#include "translation_test_support.h"
#include "snow_shot/presentation/components/screenshottranslationsettingsdialog.h"
#include "widgets/modal.h"

#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/maincontentheaderwidget.h"
#include "snow_shot/presentation/components/sidebarwidget.h"
#include "snow_shot/presentation/components/translationpagewidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/translationpagecontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/translation/translationlanguages.h"
#include "widgets/button.h"
#include "widgets/input_text_edit.h"
#include "widgets/navigation_menu.h"
#include "widgets/context_menu.h"
#include "widgets/scroll_area.h"
#include "widgets/select.h"
#include "widgets/spin.h"
#include "widgets/tabs.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
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
#include <memory>

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

void sharedServiceSelectors() {
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    const QString customKey = QStringLiteral("api_configuration/custom_models");
    const auto previousModels = configuration.value(customKey);
    auto previousPreferences = snow_shot::storage::ScreenshotTranslationSettings().configuration();
    // The schema's unset default target is not a valid explicit selection to restore.
    if (previousPreferences.targetLanguage.isEmpty())
        previousPreferences.targetLanguage =
            snow_shot::translation::defaultTranslationTargetLanguage(
                snow_shot::presentation::LanguageManager::instance().currentLocale());
    Server server;
    server.holdModels = true;
    snow_shot::CustomAiModelConfiguration model{
        QStringLiteral("22222222-2222-4222-8222-222222222222"),
        QStringLiteral("Custom vision translator"),
        server.url() + QStringLiteral("/v1"),
        {},
        QStringLiteral("provider-model"),
        true};
    require(configuration.setValue(customKey, snow_shot::customAiModelsToJson({model})),
            "configure custom vision model");
    SnowShotApiClient client(server.url());
    TranslationPageWidget page(nullptr, &client, 0);
    auto& service = snow_shot::translation::TranslationService::forClient(client, configuration,
                                                                          QLocale::English);
    require(service.savePreferences(
                {QStringLiteral("auto"), QStringLiteral("ja"), model.selectionId()}),
            "select shared custom model");
    auto* modal = snow_shot::presentation::createScreenshotTranslationSettingsDialog(service, &page,
                                                                                     &page, {});
    auto* pageSelect = child<AdSelect>(page, "translationService");
    auto* screenshotSelect =
        child<AdSelect>(*modal->contentWidget(), "screenshotTranslationService");
    const auto compare = [&] {
        const auto first = pageSelect->options();
        const auto second = screenshotSelect->options();
        require(first.size() == second.size(), "both translation views expose the same catalog");
        for (int i = 0; i < first.size(); ++i)
            require(first[i].value == second[i].value && first[i].label == second[i].label &&
                        first[i].group == second[i].group,
                    "service identity, label, and group match across views");
    };
    compare();
    require(pageSelect->isEnabled() && screenshotSelect->isEnabled() &&
                pageSelect->currentValue().toString() == model.selectionId() &&
                screenshotSelect->currentValue().toString() == model.selectionId(),
            "custom vision service is selectable in both views during builtin discovery");
    model.name = QStringLiteral("Renamed custom translator");
    require(configuration.setValue(customKey, snow_shot::customAiModelsToJson({model})),
            "rename configured model");
    compare();
    require(pageSelect->options().first().label == model.name,
            "open selectors reflect model edits");
    waitUntil([&] { return server.modelRequests == 1; }, "both views share one pending discovery");
    server.respondModels();
    waitUntil([&] { return !service.loadingModels(); }, "finish discovery for both views");
    compare();
    require(service.savePreferences(
                {QStringLiteral("en"), QStringLiteral("fr"), QStringLiteral("specialist")}),
            "change shared preferences while the settings dialog is open");
    require(pageSelect->currentValue() == screenshotSelect->currentValue() &&
                child<AdSelect>(*modal->contentWidget(), "screenshotTranslationTargetLanguage")
                        ->currentValue()
                        .toString() == QStringLiteral("fr"),
            "unedited dialog fields follow committed shared preferences");
    require(service.savePreferences(
                {QStringLiteral("auto"), QStringLiteral("ja"), model.selectionId()}),
            "restore selected custom model before deletion");
    require(configuration.setValue(customKey, QJsonArray{}), "remove selected model");
    compare();
    require(pageSelect->currentValue() == screenshotSelect->currentValue() &&
                pageSelect->currentValue().toString() == QStringLiteral("general"),
            "both views resolve the same fallback after deletion");
    screenshotSelect->setCurrentValue(QStringLiteral("specialist"));
    require(service.savePreferences(
                {QStringLiteral("en"), QStringLiteral("de"), QStringLiteral("general")}),
            "commit preferences while the screenshot dialog has a model draft");
    require(screenshotSelect->currentValue().toString() == QStringLiteral("specialist") &&
                pageSelect->currentValue().toString() == QStringLiteral("general"),
            "an uncommitted dialog edit remains local until OK");
    modal->reject();
    flushEvents();
    require(configuration.setValue(customKey, previousModels), "restore custom models");
    require(
        snow_shot::storage::ScreenshotTranslationSettings().setConfiguration(previousPreferences),
        "restore shared preferences");
}

void screenshotSettingsProviderLifetime() {
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    Server server;
    auto client = std::make_unique<SnowShotApiClient>(server.url());
    auto& service = snow_shot::translation::TranslationService::forClient(*client, configuration,
                                                                          QLocale::English);
    QWidget owner;
    const QPointer<adqt::widgets::AdModal> modal =
        snow_shot::presentation::createScreenshotTranslationSettingsDialog(service, &owner, &owner,
                                                                           {});
    auto* body = modal->contentWidget();
    auto* retry = child<AdButton>(*body, "screenshotTranslationSettingsRetry");
    client.reset();
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(body, &languageChange);
    retry->clicked();
    modal->closeRequested(adqt::widgets::AdModal::CloseReason::OkAction);
    flushEvents();
    require(modal == nullptr,
            "provider destruction safely disposes the screenshot settings dialog");
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

class CloseTrackingWindow final : public QWidget {
  public:
    int closeCount = 0;

  protected:
    void closeEvent(QCloseEvent* event) override {
        ++closeCount;
        QWidget::closeEvent(event);
    }
};

void editorContentGeometry() {
    Server server;
    SnowShotApiClient client(server.url());
    TranslationPageWidget page(nullptr, &client, 0);
    page.deactivate();
    page.resize(900, 650);
    page.show();
    flushEvents();
    auto* source = child<AdTextEdit>(page, "translationSourceText");
    auto* result = child<AdTextEdit>(page, "translationResultText");
    for (auto* editor : {source, result}) {
        const int baseline = editor->height();
        require(baseline == editor->minimumSizeHint().height(),
                "empty editor uses its minimum rows without filling spare page space");
        editor->setPlainText(QStringLiteral("A line of text.\n").repeated(80));
        waitUntil(
            [&] {
                return editor->height() > baseline && editor->verticalScrollBar()->maximum() == 0;
            },
            "multiline text expands the editor without internal scrolling");
        editor->clear();
        waitUntil([&] { return editor->height() == baseline; },
                  "clearing text restores the minimum height");
        editor->setPlainText(QString(4500, u'W'));
        flushEvents();
        const int wideHeight = editor->height();
        page.resize(300, 650);
        waitUntil(
            [&] {
                return editor->height() > wideHeight && editor->verticalScrollBar()->maximum() == 0;
            },
            "narrow layouts grow to fit wrapped text");
        editor->clear();
        page.resize(900, 650);
        flushEvents();
    }
}

void selectedTextHandoff() {
    Server server;
    SnowShotApiClient client(server.url());
    TranslationPageWidget page(nullptr, &client, 0);
    auto* source = child<AdTextEdit>(page, "translationSourceText");
    auto* controller = page.findChild<snow_shot::presentation::TranslationPageController*>();
    waitUntil([&]() { return !controller->loadingModels(); },
              "load selected text translation models");
    const auto preferences = controller->preferences();
    const QString selected =
        QString::fromUtf8("  selected \xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x8c\x8d\r\nsecond line  ");
    controller->setComposing(true);
    page.setSourceText(selected);
    const QString normalized =
        QString(selected).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    require(source->toPlainText() == normalized && controller->sourceText() == normalized,
            "handoff fills source and controller using the editor newline policy");
    waitUntil([&]() { return server.streams.size() == 1; },
              "handoff ends previous composition and automatically translates");
    require(QJsonDocument(server.streams.first().body).toJson().contains("selected"),
            "translation request contains captured source");
    server.delta(0, QStringLiteral("old result"));
    waitUntil([&]() { return controller->resultText() == QStringLiteral("old result"); },
              "first request streams a result");
    page.setSourceText(QStringLiteral("replacement"));
    require(controller->resultText().isEmpty(), "replacing source clears obsolete translation");
    server.delta(0, QStringLiteral("stale"));
    waitUntil([&]() { return server.streams.size() == 2 && server.disconnected(0); },
              "replacement cancels the old stream and starts one new translation");
    server.delta(1, QStringLiteral("new result"));
    waitUntil([&]() { return controller->resultText() == QStringLiteral("new result"); },
              "cancelled request cannot replace the new result");
    page.setSourceText(QStringLiteral("replacement"));
    flushEvents();
    require(server.streams.size() == 2 &&
                controller->resultText() == QStringLiteral("new result") &&
                controller->preferences().sourceLanguage == preferences.sourceLanguage &&
                controller->preferences().targetLanguage == preferences.targetLanguage &&
                controller->preferences().modelId == preferences.modelId,
            "identical handoff is a no-op and language/model preferences are preserved");
    const QString boundary = QString(4999, u'a') + QString::fromUcs4(U"\U0001f30d");
    page.setSourceText(boundary + QStringLiteral("overflow"));
    require(source->toPlainText() == boundary && controller->sourceText() == boundary,
            "selected text observes the same 5000-code-point limit as pasted text");
    page.deactivate();
    page.setSourceText(QStringLiteral("late"));
    require(controller->sourceText().isEmpty(), "deactivated page rejects a late handoff");
}

void selectedTextNavigation() {
    Server server;
    qputenv("SNOW_SHOT_API_BASE_URL", server.url().toUtf8());
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession runtime(settings::builtInSettingsRegistry(), backend);
    MainWindow window(settings::builtInSettingsRegistry(), runtime);
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    auto* card = window.findChild<ContentCardWidget*>();
    auto* sidebar = window.findChild<SidebarWidget*>();
    window.showTranslation(QStringLiteral("first selection"));
    auto* page = window.findChild<TranslationPageWidget*>();
    require(window.isVisible() && page != nullptr &&
                sidebar->currentRoute() == QStringLiteral("/tools/translation") &&
                card->currentRoute() == QStringLiteral("/tools/translation") &&
                child<AdTextEdit>(*page, "translationSourceText")->toPlainText() ==
                    QStringLiteral("first selection"),
            "hidden main window opens Translation and synchronizes navigation and source");
    window.showMinimized();
    window.showTranslation(QStringLiteral("second selection"));
    require(!window.isMinimized() && window.findChild<TranslationPageWidget*>() == page &&
                child<AdTextEdit>(*page, "translationSourceText")->toPlainText() ==
                    QStringLiteral("second selection"),
            "handoff restores minimized window and reuses an active translation page");
    card->setCurrentRoute(QStringLiteral("/global-hotkeys"));
    flushEvents();
    window.hide();
    window.showTranslation(QStringLiteral("third selection"));
    page = window.findChild<TranslationPageWidget*>();
    require(window.isVisible() && page != nullptr &&
                child<AdTextEdit>(*page, "translationSourceText")->toPlainText() ==
                    QStringLiteral("third selection"),
            "handoff creates a new translation page after navigation disposed the previous one");
    window.close();
    qunsetenv("SNOW_SHOT_API_BASE_URL");
}

void editorAndShortcutBehavior() {
    Server server;
    SnowShotApiClient client(server.url());
    CloseTrackingWindow owner;
    owner.resize(700, 650);
    auto* page = new TranslationPageWidget(&owner, &client, 0);
    page->setGeometry(owner.rect());
    QObject::connect(page, &TranslationPageWidget::closeWindowRequested, &owner, &QWidget::close);
    owner.show();
    auto* source = child<AdTextEdit>(*page, "translationSourceText");
    auto* result = child<AdTextEdit>(*page, "translationResultText");
    auto* spin = child<AdSpin>(*page, "translationResultSpin");
    require(!spin->spinning() && spin->isHidden(), "idle translation has no loading indicator");
    auto* copy = child<QAction>(*page, "translationCopy");
    auto* copyClose = child<QAction>(*page, "translationCopyAndClose");
    auto* floating = child<AdButton>(*page, "translationActions");
    auto* menu = child<AdContextMenu>(*page, "translationActionsMenu");
    require(!menu->actionIcon(copy).isValid() && !menu->actionIcon(copyClose).isValid(),
            "translation menu actions have no leading icons");
    auto* controller = page->findChild<snow_shot::presentation::TranslationPageController*>();
    require(controller != nullptr && result->isReadOnly() && !copy->isEnabled() &&
                !child<AdButton>(*page, "translationResultCopy")->isEnabled(),
            "empty page has read-only result and disabled copy actions");
    QApplication::clipboard()->setText(QStringLiteral("sentinel"));
    key(source, Qt::Key_Q, Qt::ControlModifier);
    require(owner.closeCount == 0 && owner.isVisible() &&
                QApplication::clipboard()->text() == QStringLiteral("sentinel"),
            "empty Copy and Close leaves clipboard and window intact");

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
    require(spin->spinning() && spin->isVisible(),
            "Spin appears while waiting for the first token");
    server.delta(0, QStringLiteral("你好，"));
    waitUntil([&]() { return result->toPlainText() == QStringLiteral("你好，"); },
              "show partial result");
    require(copy->isEnabled() && copyClose->isEnabled(), "partial results are copyable");
    require(spin->spinning() && spin->isVisible(), "Spin stays visible while tokens stream");
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
    copy->trigger();
    require(QApplication::clipboard()->text() == result->toPlainText(),
            "floating Copy always copies the whole result despite a selection");

    const QPointF local = floating->rect().center();
    QCursor::setPos(floating->mapToGlobal(local.toPoint()));
    QEnterEvent hover(local, local, floating->mapToGlobal(local.toPoint()));
    QApplication::sendEvent(floating, &hover);
    waitUntil([&]() { return menu->isVisible(); }, "hover reveals actions");
    require(menu->geometry().bottom() < floating->mapToGlobal(QPoint()).y(),
            "translation actions open above the floating trigger");
    require(menu->triggerWidget() == floating && menu->actions().size() == 2,
            "translation uses the shared two-action context menu");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(floating, &leave);
    QCursor::setPos(menu->mapToGlobal(menu->actionGeometry(copy).center()));
    QEnterEvent actionHover(QPointF(5, 5), QPointF(5, 5),
                            menu->mapToGlobal(menu->actionGeometry(copy).center()));
    QApplication::sendEvent(menu, &actionHover);
    flushEvents();
    require(menu->isVisible(), "pointer can travel from floating button to action");
    key(menu, Qt::Key_Escape);
    require(!menu->isVisible() && floating->hasFocus(),
            "Escape dismisses the action popup and restores trigger focus");
    QCursor::setPos(owner.mapToGlobal(QPoint(2, 2)));
    flushEvents();
    key(floating, Qt::Key_Return);
    require(menu->isVisible() && menu->activeAction() == copy,
            "keyboard activation reveals actions and selects Copy");
    QApplication::sendEvent(menu, &leave);
    QEventLoop settle;
    QTimer::singleShot(200, &settle, &QEventLoop::quit);
    settle.exec();
    require(menu->isVisible(), "keyboard navigation does not require pointer hover");
    key(menu, Qt::Key_Escape);
    key(floating, Qt::Key_Space);
    require(menu->isVisible(), "Space also reveals the actions");
    QApplication::clipboard()->setText(QStringLiteral("before keyboard action"));
    key(menu, Qt::Key_Return);
    require(!menu->isVisible() && QApplication::clipboard()->text() == result->toPlainText(),
            "Enter activates a focused popup action");
    floating->click();
    require(menu->isVisible(), "click opens the same action menu");
    key(menu, Qt::Key_C, Qt::ControlModifier);
    require(!menu->isVisible() && QApplication::clipboard()->text() == result->toPlainText(),
            "Ctrl+C works while the context menu owns focus");
    floating->click();
    owner.hide();
    require(!menu->isVisible(), "hiding the owner dismisses the action menu");
    owner.show();
    const int shortResultHeight = result->height();
    server.delta(0, QStringLiteral("\nA line of translated text.").repeated(80));
    waitUntil(
        [&]() {
            return result->height() > shortResultHeight &&
                   result->verticalScrollBar()->maximum() == 0;
        },
        "long results grow without internal scrolling");
    auto* pageScroll = page->findChild<AdScrollArea*>();
    waitUntil([&] { return pageScroll->verticalScrollBar()->maximum() > 0; },
              "long translations scroll at page level");
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
    require(owner.closeCount == 1 && !owner.isVisible() &&
                QApplication::clipboard()->text() == result->toPlainText(),
            "Ctrl+Q copies the partial result and closes the owning window");
    server.finish(0);
    waitUntil([&]() { return !controller->translating(); }, "hidden page finishes its stream");
    require(!spin->spinning() && spin->isHidden(), "completion stops and hides Spin");
    owner.show();
    require(!source->toPlainText().isEmpty() && !result->toPlainText().isEmpty(),
            "non-deleting test owner retains its draft after closing");
    floating->click();
    require(menu->isVisible(), "actions reopen before deactivation");
    page->deactivate();
    require(!menu->isVisible(), "deactivation dismisses the action menu");
    source->setPlainText(QStringLiteral("after deactivation"));
    QApplication::clipboard()->setText(QStringLiteral("untouched"));
    key(source, Qt::Key_Q, Qt::ControlModifier);
    require(owner.isVisible() && QApplication::clipboard()->text() == QStringLiteral("untouched"),
            "inactive page cannot execute translation shortcuts");
}

void languageDropdowns() {
    Server server;
    SnowShotApiClient client(server.url());
    TranslationPageWidget page(nullptr, &client, 0);
    page.deactivate();
    page.resize(900, 650);
    page.show();
    flushEvents();
    for (const char* name : {"translationSourceLanguage", "translationTargetLanguage"}) {
        auto* select = child<AdSelect>(page, name);
        require(select->popupLayerMode() == AdSelect::PopupLayerMode::QtTool,
                "language dropdowns use the screenshot settings tool layer");
        for (const auto& option : select->options()) {
            const QString code = option.value.toString();
            require(option.group ==
                        (code == QStringLiteral("auto") ? QString() : code.left(1).toUpper()),
                    "languages use code-initial groups with auto-detect outside the groups");
        }
        select->setPopupVisible(true);
        flushEvents();
        QWidget* popup = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QStringLiteral("adselect-popup") && widget->isVisible()) {
                popup = widget;
                break;
            }
        }
        require(popup != nullptr && popup->windowType() == Qt::Tool,
                "opening a language dropdown displays a separate tool window");
        select->setPopupVisible(false);
        flushEvents();
        require(!popup->isVisible(), "closing the language dropdown hides its tool window");
    }
}

void selectorContentGeometry() {
    AdSelect sizing;
    sizing.setOptions({{QStringLiteral("en"), QStringLiteral("English")},
                       {QStringLiteral("zh"), QStringLiteral("Simplified Chinese")}});
    sizing.setCurrentValue(QStringLiteral("en"));
    const QSize fixedHint = sizing.sizeHint();
    sizing.setCurrentValue(QStringLiteral("zh"));
    require(sizing.sizeHint() == fixedHint, "default Select sizing remains compatible");
    sizing.setSizeAdjustPolicy(AdSelect::SizeAdjustPolicy::AdjustToCurrentText);
    const int longHint = sizing.sizeHint().width();
    sizing.setCurrentValue(QStringLiteral("en"));
    require(sizing.sizeHint().width() < longHint, "content policy measures the selected label");
    const int unprefixed = sizing.sizeHint().width();
    sizing.setPrefixText(QStringLiteral("Language"));
    require(sizing.sizeHint().width() > unprefixed, "content sizing includes a visible prefix");
    sizing.setPrefixText({});
    require(sizing.sizeHint().width() == unprefixed, "removing prefix releases its width");
    sizing.setCurrentValue({});
    sizing.setPlaceholder(QStringLiteral("Choose"));
    const int placeholderWidth = sizing.sizeHint().width();
    sizing.setPlaceholder(QStringLiteral("Choose a translation language"));
    require(sizing.sizeHint().width() > placeholderWidth, "empty select measures its placeholder");
    sizing.setMode(AdSelect::Mode::Multiple);
    require(sizing.sizeHint().width() == fixedHint.width(),
            "multiple selection keeps wrapping sizing");

    Server server;
    SnowShotApiClient client(server.url());
    TranslationPageWidget page(nullptr, &client, 0);
    page.resize(1100, 650);
    page.show();
    auto* controller = page.findChild<snow_shot::presentation::TranslationPageController*>();
    waitUntil([&]() { return !controller->loadingModels(); }, "load selector geometry fixture");
    auto* scroll = page.findChild<AdScrollArea*>();
    require(scroll->viewport()->mapTo(&page, scroll->viewport()->rect().bottomLeft()).y() ==
                page.rect().bottom(),
            "floating shortcuts do not reserve a footer below the scroll viewport");
    auto* target = child<AdSelect>(page, "translationTargetLanguage");
    auto* swap = child<AdButton>(page, "translationSwap");
    target->setCurrentValue(QStringLiteral("en"));
    flushEvents();
    flushEvents();
    const int shortWidth = target->width();
    const int gap =
        target->mapTo(&page, QPoint()).x() - swap->mapTo(&page, QPoint(swap->width(), 0)).x();
    target->setCurrentValue(QStringLiteral("zh-Hans"));
    flushEvents();
    flushEvents();
    require(target->width() > shortWidth, "selector grows to fit a longer selected label");
    require(gap >= 0 &&
                gap <= styles::ThemeManager::instance().themeColorScheme().metricAlias.paddingXXS,
            "target language follows the swap button without unused column space");
    target->setCurrentValue(QStringLiteral("en"));
    flushEvents();
    flushEvents();
    require(target->width() == shortWidth, "selector shrinks when returning to a shorter label");
    for (const char* name :
         {"translationSourceLanguage", "translationTargetLanguage", "translationService"}) {
        auto* select = child<AdSelect>(page, name);
        require(select->width() == select->sizeHint().width(),
                "all translation selectors use their content width");
    }
    auto* service = child<AdSelect>(page, "translationService");
    auto* result = child<AdTextEdit>(page, "translationResultText");
    require(service->mapTo(&page, QPoint(service->width(), 0)).x() ==
                result->mapTo(&page, QPoint(result->width(), 0)).x(),
            "translation service aligns with the right edge of the result pane");
    page.resize(320, 650);
    flushEvents();
    flushEvents();
    auto* source = child<AdSelect>(page, "translationSourceLanguage");
    require(source->mapTo(&page, QPoint()).x() == target->mapTo(&page, QPoint()).x(),
            "stacked source and target selectors share the same left edge");
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
                                      QStringLiteral("/history"),
            "Translation follows Screenshot history in navigation");
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
                require(
                    child<QAction>(*page, "translationCopyAndClose")->text() ==
                        translator.translate("TranslationPageWidget", "Copy and Close (Ctrl+Q)"),
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
                        require(viewportGeometry.contains(floating->geometry()) &&
                                    viewportGeometry.bottom() == page->rect().bottom(),
                                "shortcut button floats over a viewport that fills the page");
                        require(floating->size() == QSize(32, 32),
                                "shortcut button uses the smaller 32-pixel size");
                        require(page->childAt(floating->geometry().center()) == floating,
                                "shortcut button stays above the scrolling content");
                        const auto* inlineCopy = child<AdButton>(*page, "translationResultCopy");
                        require(source->width() > 100 && result->width() > 100,
                                "both text panes remain usable at minimum window size");
                        const auto* spin = child<AdSpin>(*page, "translationResultSpin");
                        require(spin->spinning() == controller->translating() &&
                                    spin->isHidden() != controller->translating(),
                                "Spin follows translation state across theme and layout changes");
                        if (controller->translating()) {
                            const QRect spinRect(spin->pos() - result->pos(), spin->size());
                            const auto* copy = child<AdButton>(*page, "translationResultCopy");
                            const QRect copyRect(copy->pos() - result->pos(), copy->size());
                            if (!result->rect().contains(spinRect)) {
                                std::cerr << "Spin outside result: locale=" << locale.toStdString()
                                          << " window=" << size.width() << 'x' << size.height()
                                          << " result=" << result->width() << 'x'
                                          << result->height() << " spin=" << spinRect.x() << ','
                                          << spinRect.y() << ' ' << spinRect.width() << 'x'
                                          << spinRect.height() << '\n';
                            }
                            require(result->rect().contains(spinRect) &&
                                        spinRect.center().x() < result->width() / 2 &&
                                        spinRect.center().y() > result->height() / 2,
                                    "Spin overlays the lower-left corner of the result");
                            require(copy->isHidden() || !spinRect.intersects(copyRect),
                                    "streaming Spin leaves the inline copy button reachable");
                        }
                        require(source->height() >= source->minimumSizeHint().height() &&
                                    result->height() >= result->minimumSizeHint().height() &&
                                    source->minimumVisibleRows() == 10 &&
                                    result->minimumVisibleRows() == 10,
                                "editor geometry retains its 10-row baseline after updates");
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
                                const QRect copyGeometry(
                                    inlineCopy->mapTo(scroll->viewport(), QPoint()),
                                    inlineCopy->size());
                                require(scroll->viewport()->rect().contains(copyGeometry),
                                        "scrolling to the bottom makes the inline copy fully "
                                        "reachable");
                                require(
                                    !QRect(inlineCopy->mapTo(page, QPoint()), inlineCopy->size())
                                         .intersects(floating->geometry()),
                                    "shortcut overlay does not cover copy after scrolling");
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
    const QRect actionGeometry = actions->geometry();
    source->clearFocus();
    flushEvents();
    source->setFocus();
    flushEvents();
    require(actions->geometry() == actionGeometry &&
                page->childAt(actionGeometry.center()) == actions,
            "focusing the editor leaves the shortcut overlay fixed and reachable");
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
    window.hide();
    require(!window.isVisible(), "ordinary hiding leaves the main window available");
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
    for (const bool useShortcut : {false, true}) {
        QPointer<MainWindow> closing = new MainWindow(settings::builtInSettingsRegistry(), runtime);
        closing->show();
        closing->findChild<ContentCardWidget*>()->setCurrentRoute(
            QStringLiteral("/tools/translation"));
        QPointer<TranslationPageWidget> closingPage = closing->findChild<TranslationPageWidget*>();
        require(closingPage != nullptr, "closing window owns a translation page");
        auto* closingSource = child<AdTextEdit>(*closingPage, "translationSourceText");
        auto* closingResult = child<AdTextEdit>(*closingPage, "translationResultText");
        const int streamIndex = static_cast<int>(server.streams.size());
        closingSource->setPlainText(QStringLiteral("Discard me"));
        waitUntil([&] { return server.streams.size() == streamIndex + 1; },
                  "start translation in the window to close");
        server.delta(streamIndex, QStringLiteral("Copied before closing"));
        waitUntil([&] { return !closingResult->toPlainText().isEmpty(); },
                  "receive a partial translation before closing");
        if (useShortcut)
            key(closingSource, Qt::Key_Q, Qt::ControlModifier);
        else
            child<QAction>(*closingPage, "translationCopyAndClose")->trigger();
        flushEvents();
        require(
            closing.isNull() && closingPage.isNull() &&
                QApplication::clipboard()->text() == QStringLiteral("Copied before closing"),
            "Copy and Close and Ctrl+Q copy the result and destroy the current window and draft");
    }
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
    QObject::connect(page, &TranslationPageWidget::closeWindowRequested, &owner, &QWidget::close);
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
    auto* menu = child<AdContextMenu>(*page, "translationActionsMenu");
    QCursor::setPos(floating->mapToGlobal(floating->rect().center()));
    waitUntil([&]() { return menu->isVisible(); }, "native pointer hover opens the popup");
    snapshot(owner, QStringLiteral("translation-native-hover"));
    auto nativeClick = [menu](QAction* action) {
        QCursor::setPos(menu->mapToGlobal(menu->actionGeometry(action).center()));
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
    nativeClick(child<QAction>(*page, "translationCopy"));
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
    waitUntil([&]() { return !owner.isVisible(); }, "native Ctrl+Q closes the window");
    server.finish(0);
    owner.show();
    owner.raise();
    owner.activateWindow();
    waitUntil(activate, "reactivate the native translation test after hiding");
    QCursor::setPos(owner.mapToGlobal(QPoint(8, 8)));
    flushEvents();
    QCursor::setPos(floating->mapToGlobal(floating->rect().center()));
    waitUntil([&]() { return menu->isVisible(); }, "native hover works after hide and reopen");
    nativeClick(child<QAction>(*page, "translationCopyAndClose"));
    waitUntil([&]() { return !owner.isVisible(); },
              "native Copy and Close action closes the window");
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
    if (app.arguments().contains(QStringLiteral("--language-dropdowns"))) {
        languageDropdowns();
        storage.shutdown();
        return 0;
    }
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--native-interaction"))) {
        nativeWindowInteraction();
    } else
#endif
    {
        sharedServiceSelectors();
        screenshotSettingsProviderLifetime();
        selectedTextHandoff();
        selectedTextNavigation();
        editorContentGeometry();
        languageDropdowns();
        selectorContentGeometry();
        editorAndShortcutBehavior();
        navigationThemesLanguagesAndGeometry();
    }
    storage.shutdown();
    return 0;
}
