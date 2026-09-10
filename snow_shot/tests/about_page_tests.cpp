#include "snow_shot/presentation/components/aboutpagewidget.h"
#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/maincontentheaderwidget.h"
#include "snow_shot/presentation/components/sidebarwidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/settings/settingssearchindex.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/update/updateservice.h"

#include "widgets/button.h"
#include "widgets/button_style.h"
#include "widgets/divider.h"
#include "theme/theme.h"
#include "widgets/navigation_menu.h"
#include "widgets/scroll_area.h"
#include "widgets/tabs.h"

#include <QApplication>
#include <QAbstractButton>
#include <QClipboard>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QImage>
#include <QFontDatabase>
#include <QLabel>
#include <QKeyEvent>
#include <QPointer>
#include <QProgressBar>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

namespace settings = snow_shot::presentation::settings;
namespace styles = snow_shot::presentation::styles;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void flushEvents() {
    // A resize can enqueue a second layout pass after the responsive grid has changed columns.
    for (int pass = 0; pass < 4; ++pass) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

template <typename T> T* child(QObject& parent, const char* name) {
    auto* result = parent.findChild<T*>(QString::fromLatin1(name));
    require(result != nullptr, name);
    return result;
}

void snapshot(QWidget& widget, const QString& name) {
    const QString directory = qEnvironmentVariable("SNOW_SHOT_ABOUT_SNAPSHOT_DIR");
    if (!directory.isEmpty()) {
        require(QDir().mkpath(directory), "create About preview directory");
        require(widget.grab().save(QDir(directory).filePath(name + QStringLiteral(".png"))),
                "save About preview");
    }
}

void versionIsExactSelectableAndCopyable() {
    const QString version = QStringLiteral("12.34.56-beta.7+build.89");
    QCoreApplication::setApplicationVersion(version);
    AboutPageWidget page;
    auto* value = child<QLabel>(page, "aboutVersionValue");
    auto* copy = child<adqt::widgets::AdButton>(page, "aboutCopyVersion");
    require(value->text() == version, "display the complete version including prerelease metadata");
    require(value->accessibleName().contains(version), "screen readers can identify the version");
    require(value->textFormat() == Qt::PlainText &&
                value->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard) &&
                value->textInteractionFlags().testFlag(Qt::TextSelectableByMouse),
            "version is plain text and selectable with mouse and keyboard");
    require(copy->isEnabled() && copy->focusPolicy() == Qt::StrongFocus,
            "copy action is keyboard accessible");
    require(!child<QLabel>(page, "aboutPreviewBadge")->isHidden(),
            "prerelease versions carry a preview badge");
    copy->click();
    require(QApplication::clipboard()->text() == version, "copy the exact displayed version");
    require(copy->text() == QStringLiteral("Copied"), "show copy feedback");
    auto* timer = child<QTimer>(page, "aboutCopyFeedbackTimer");
    require(timer != nullptr && timer->isActive(), "copy feedback expires");
    timer->stop();
    require(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
            "simulate feedback timeout without waiting");
    require(copy->text() == QStringLiteral("Copy version number"),
            "restore the copy action after feedback");
}

void absentVersionDoesNotInventARelease() {
    for (const QString& version : {QString(), QStringLiteral("   ")}) {
        QCoreApplication::setApplicationVersion(version);
        AboutPageWidget page;
        require(child<QLabel>(page, "aboutVersionValue")->text() == QStringLiteral("Unavailable"),
                "missing version uses a translated fallback");
        require(!child<adqt::widgets::AdButton>(page, "aboutCopyVersion")->isEnabled(),
                "missing version cannot be copied");
        require(child<QLabel>(page, "aboutPreviewBadge")->isHidden(),
                "missing versions do not claim to be preview releases");
    }
}

void stableVersionsDoNotClaimToBePreviews() {
    for (const QString& version :
         {QStringLiteral("1.0.0"), QStringLiteral("1.0.0+build-beta.89")}) {
        QCoreApplication::setApplicationVersion(version);
        AboutPageWidget page;
        require(child<QLabel>(page, "aboutPreviewBadge")->isHidden(),
                "stable releases remain stable even with hyphens in build metadata");
        require(child<QLabel>(page, "aboutVersionValue")->text() == version,
                "stable releases preserve build metadata");
    }
}

void projectLinkSurfacesMatchStandardButtons() {
    using adqt::widgets::AdButton;
    auto& manager = styles::ThemeManager::instance();
    AboutPageWidget page;
    page.resize(700, 540);
    page.show();
    flushEvents();
    const auto surface = [](QWidget& widget, qreal dpr) {
        QImage image(QSize(qRound(widget.width() * dpr), qRound(widget.height() * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        widget.render(&image, QPoint(), QRegion(), QWidget::DrawWindowBackground);
        return image;
    };
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        manager.setThemeAppearance(appearance);
        flushEvents();
        for (const char* name : {"aboutWebsite", "aboutSourceCode", "aboutFeedback"}) {
            auto* button = child<AdButton>(page, name);
            require(button->buttonStyle() == AdButton::ButtonStyle::Outline &&
                        button->accentRole() == AdButton::AccentRole::Neutral,
                    "resource links use standard neutral outline buttons");
            AdButton reference(button->parentWidget());
            reference.resize(button->size());
            reference.show();
            reference.clearFocus();
            button->clearFocus();
            for (const int state : {0, 1, 2, 3}) {
                for (AdButton* target : {button, &reference}) {
                    target->setEnabled(state != 3);
                    target->setDown(state == 2);
                    if (state == 1) {
                        QEnterEvent enter(QPointF(2, 2), QPointF(2, 2), QPointF(2, 2));
                        QCoreApplication::sendEvent(target, &enter);
                    } else {
                        QEvent leave(QEvent::Leave);
                        QCoreApplication::sendEvent(target, &leave);
                    }
                }
                for (const qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
                    require(surface(*button, dpr) == surface(reference, dpr),
                            "resource link surfaces match AdButton in both themes, all "
                            "interaction states and fractional scales");
                }
                adqt::widgets::detail::ButtonStyleInput input;
                input.buttonStyle = reference.buttonStyle();
                input.accentRole = reference.accentRole();
                input.sizeClass = reference.sizeClass();
                input.baseFont = reference.font();
                const auto visual = adqt::widgets::detail::resolveButtonVisualStyle(
                    input, adqt::theme::ThemeManager::instance().resolve(&reference));
                const QColor expected = state == 3   ? visual.disabled.text
                                        : state == 2 ? visual.active.text
                                        : state == 1 ? visual.hover.text
                                                     : visual.normal.text;
                for (const auto* suffix : {"Title", "Description"}) {
                    auto* label = button->findChild<QLabel*>(QString::fromLatin1(name) +
                                                             QString::fromLatin1(suffix));
                    require(label != nullptr &&
                                label->palette().color(label->foregroundRole()) == expected,
                            "resource link titles and descriptions follow AdButton text colors "
                            "in both themes and every interaction state");
                }
            }
            button->setEnabled(true);
            button->setDown(false);
        }
    }
    manager.setThemeAppearance(styles::ThemeAppearance::Light);
}

void projectLinksAreExplicitAccessibleAndRecoverable() {
    QList<QUrl> opened;
    bool canOpen = true;
    AboutPageWidget page(nullptr, [&](const QUrl& url) {
        opened.append(url);
        return canOpen;
    });
    page.resize(700, 540);
    page.show();
    flushEvents();
    require(opened.isEmpty(), "About does not open links or check for updates on construction");
    snapshot(page, QStringLiteral("about-actions"));
    const QString project = QStringLiteral(SNOW_SHOT_TEST_PROJECT_URL);
    const std::array<std::pair<const char*, QUrl>, 4> links{{
        {"aboutWebsite", QUrl(QStringLiteral(SNOW_SHOT_TEST_WEBSITE_URL))},
        {"aboutSourceCode", QUrl(project)},
        {"aboutFeedback", QUrl(project + QStringLiteral("/issues"))},
        {"aboutReleaseNotes", QUrl(project + QStringLiteral("/releases"))},
    }};
    for (const auto& [name, url] : links) {
        auto* button = child<QAbstractButton>(page, name);
        require(button->focusPolicy() == Qt::StrongFocus && !button->accessibleName().isEmpty(),
                "every project action is named and keyboard accessible");
        require(button->toolTip() == url.toDisplayString(),
                "project actions expose their exact destinations");
        button->setFocus();
        for (const auto key : {Qt::Key_Space, Qt::Key_Return}) {
            const auto previousCount = opened.size();
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
            QCoreApplication::sendEvent(button, &press);
            QCoreApplication::sendEvent(button, &release);
            require(opened.size() == previousCount + 1 && opened.back() == url,
                    "Space and Enter open exactly the requested project destination");
        }
    }
    page.resize(360, 240);
    flushEvents();
    canOpen = false;
    child<QAbstractButton>(page, "aboutWebsite")->click();
    flushEvents();
    auto* error = child<QLabel>(page, "aboutLinkError");
    require(!error->isHidden() && error->text().contains(links.front().second.toDisplayString()) &&
                error->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard),
            "failed browser launches show the selectable destination without a modal dialog");
    auto* scroll = child<adqt::widgets::AdScrollArea>(page, "pageScrollArea");
    require(scroll->viewport()->rect().contains(
                QRect(error->mapTo(scroll->viewport(), QPoint()), error->size())),
            "link failures are scrolled into view after the narrow layout has settled");
    for (const QString& locale :
         {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        QTranslator translator;
        require(translator.load(QStringLiteral(SNOW_SHOT_TEST_TRANSLATIONS_DIR) +
                                QStringLiteral("/snow_shot_%1.qm").arg(locale)),
                "load link failure translation");
        QCoreApplication::installTranslator(&translator);
        flushEvents();
        require(error->text() == translator
                                     .translate("AboutPageWidget",
                                                "Could not open the link. Open %1 in your browser.")
                                     .arg(links.front().second.toDisplayString()),
                "an existing link error retranslates and preserves its destination");
        QCoreApplication::removeTranslator(&translator);
        flushEvents();
    }
    canOpen = true;
    child<QAbstractButton>(page, "aboutWebsite")->click();
    require(error->isHidden() && error->text().isEmpty(), "successful retries clear link errors");
    page.hide();
}

void largerTypeKeepsEveryActionReachable() {
    auto& manager = styles::ThemeManager::instance();
    styles::ThemeStyleConfig config;
    config.fontSize = 20;
    manager.setThemeStyleConfig(config);
    QCoreApplication::setApplicationVersion(QStringLiteral("12.34.56-beta.7+build.89"));
    snow_shot::update::UpdateService updates({});
    const_cast<snow_shot::update::UpdateStatus&>(updates.status()) = {
        snow_shot::update::UpdateState::Ready, QStringLiteral("12.34.56-beta.8+build.90")};
    AboutPageWidget page(nullptr, [](const QUrl&) { return true; }, &updates);
    page.resize(360, 360);
    page.show();
    flushEvents();
    auto* scroll = child<adqt::widgets::AdScrollArea>(page, "pageScrollArea");
    require(scroll->horizontalScrollBar()->maximum() == 0,
            "larger fonts and a narrow page never require horizontal scrolling");
    auto* artwork = child<QWidget>(page, "aboutArtwork");
    require(artwork->width() <= scroll->viewport()->width() && artwork->height() > 0,
            "artwork scales down to the available width with larger fonts");
    for (const char* name : {"aboutCopyVersion", "aboutReleaseNotes", "aboutUpdateAction",
                             "aboutWebsite", "aboutSourceCode", "aboutFeedback"}) {
        auto* button = child<QAbstractButton>(page, name);
        scroll->ensureWidgetVisible(button);
        flushEvents();
        const QRect visibleButton(button->mapTo(scroll->viewport(), QPoint()), button->size());
        require(scroll->viewport()->rect().contains(visibleButton),
                "each action can be scrolled fully into view with larger fonts");
    }
    snapshot(page, QStringLiteral("about-large-type-bottom"));
    page.hide();
    manager.setThemeStyleConfig(styles::ThemeStyleConfig{});
    flushEvents();
}

void updatePolicyAndUnavailableCopy() {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto binding = settings::SettingsSelectBinding::UpdateMode;
    require(backend.selectValue(binding).toString() == QStringLiteral("download"),
            "automatic download is the default update policy");
    for (const QString& value :
         {QStringLiteral("manual"), QStringLiteral("check"), QStringLiteral("download")}) {
        require(backend.applySelectValue(binding, value) &&
                    backend.selectValue(binding).toString() == value,
                "update policy round-trips through the settings backend");
    }
    require(!backend.applySelectValue(binding, QStringLiteral("invalid")) &&
                backend.selectValue(binding).toString() == QStringLiteral("download"),
            "invalid update policy preserves the previous value");
    snow_shot::update::UpdateService updates({});
    AboutPageWidget page(nullptr, {}, &updates);
    require(!child<adqt::widgets::AdButton>(page, "aboutUpdateAction")->isEnabled(),
            "development copies cannot overwrite themselves");
    require(child<QLabel>(page, "aboutUpdateStatus")->text() ==
                QStringLiteral("Automatic updates are unavailable for this copy."),
            "unavailable update status explains the disabled action");
}

void updateStatesFitTheVersionPanel() {
    using namespace snow_shot::update;
    using adqt::widgets::AdButton;
    UpdateService updates({});
    // Inject presentation snapshots without networking, downloads, or restart side effects.
    auto& status = const_cast<UpdateStatus&>(updates.status());
    QCoreApplication::setApplicationVersion(QStringLiteral(SNOW_SHOT_TEST_VERSION));
    AboutPageWidget page(nullptr, [](const QUrl&) { return true; }, &updates);
    auto* panel = child<QFrame>(page, "aboutVersionPanel");
    auto* label = child<QLabel>(page, "aboutUpdateStatus");
    auto* action = child<AdButton>(page, "aboutUpdateAction");
    auto* cancel = child<AdButton>(page, "aboutUpdateCancel");
    auto* progress = child<QProgressBar>(page, "aboutUpdateProgress");
    auto* scroll = child<adqt::widgets::AdScrollArea>(page, "pageScrollArea");
    require(panel->isAncestorOf(label) && panel->isAncestorOf(action) &&
                panel->isAncestorOf(cancel) && panel->isAncestorOf(progress),
            "update feedback and actions belong to the current version surface");
    require(action->sizeClass() == AdButton::SizeClass::Small &&
                cancel->sizeClass() == AdButton::SizeClass::Small &&
                action->focusPolicy() == Qt::StrongFocus &&
                cancel->focusPolicy() == Qt::StrongFocus,
            "update actions match the compact, keyboard accessible version controls");
    page.show();
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        styles::ThemeManager::instance().setThemeAppearance(appearance);
        const auto colors = styles::ThemeManager::instance().themeColorScheme().map;
        for (const QString& locale :
             {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
            QTranslator translator;
            require(translator.load(QStringLiteral(SNOW_SHOT_TEST_TRANSLATIONS_DIR) +
                                    QStringLiteral("/snow_shot_%1.qm").arg(locale)),
                    "load update UI translations");
            QCoreApplication::installTranslator(&translator);
            for (const int width : {660, 360}) {
                page.resize(width, 460);
                for (const auto state :
                     {UpdateState::Unavailable, UpdateState::Idle, UpdateState::Checking,
                      UpdateState::Available, UpdateState::Downloading, UpdateState::Verifying,
                      UpdateState::Ready, UpdateState::Applying, UpdateState::Failed}) {
                    status = {state,
                              QStringLiteral("12.34.56-beta.7+build.89"),
                              {},
                              5 * 1048576,
                              20 * 1048576};
                    if (state == UpdateState::Failed) {
                        status.error = QStringLiteral("The download could not be completed. "
                                                      "Check your connection and try again.");
                    }
                    updates.statusChanged();
                    flushEvents();
                    const bool actionable =
                        state == UpdateState::Idle || state == UpdateState::Available ||
                        state == UpdateState::Ready || state == UpdateState::Failed;
                    require(action->isVisible() == actionable && action->isEnabled() == actionable,
                            "only actionable update states display a primary control");
                    require(cancel->isVisible() == (state == UpdateState::Downloading) &&
                                progress->isVisible() == (state == UpdateState::Downloading),
                            "download controls are confined to the downloading state");
                    require(action->buttonStyle() ==
                                ((state == UpdateState::Available || state == UpdateState::Ready)
                                     ? AdButton::ButtonStyle::Solid
                                     : AdButton::ButtonStyle::Outline),
                            "download and restart receive the primary visual emphasis");
                    require(!child<QLabel>(page, "aboutUpdateIcon")->pixmap().isNull(),
                            "every update state has a rendered status icon");
                    require(label->textFormat() == Qt::PlainText &&
                                label->accessibleName() == label->text(),
                            "status remains readable as plain text and through accessibility");
                    if (state == UpdateState::Failed) {
                        require(label->palette().color(QPalette::WindowText) ==
                                    colors.colorErrorText,
                                "update errors follow the active theme's error color");
                    }
                    require(scroll->horizontalScrollBar()->maximum() == 0,
                            "all update states and languages fit without horizontal scrolling");
                    if (label->height() < label->heightForWidth(label->width())) {
                        std::cerr << "Clipped update " << static_cast<int>(state) << ' ' << width
                                  << ' ' << locale.toStdString() << ": " << label->width() << 'x'
                                  << label->height() << " required "
                                  << label->heightForWidth(label->width()) << '\n';
                        std::cerr << "Panel width/minimum/hfw/height " << panel->width() << ' '
                                  << panel->minimumSizeHint().width() << ' '
                                  << panel->heightForWidth(panel->width()) << ' ' << panel->height()
                                  << '\n';
                        scroll->ensureWidgetVisible(label);
                        flushEvents();
                        snapshot(page, QStringLiteral("about-update-clipped"));
                    }
                    require(label->height() >= label->heightForWidth(label->width()),
                            "wrapped update status text is not clipped");
                    for (auto* button : {action, cancel}) {
                        if (!button->isVisible()) {
                            continue;
                        }
                        scroll->ensureWidgetVisible(button);
                        flushEvents();
                        require(scroll->viewport()->rect().contains(QRect(
                                    button->mapTo(scroll->viewport(), QPoint()), button->size())),
                                "update actions can be scrolled fully into view");
                        const QRect statusRect(label->mapTo(panel, QPoint()), label->size());
                        const QRect buttonRect(button->mapTo(panel, QPoint()), button->size());
                        require(panel->rect().contains(buttonRect) &&
                                    !statusRect.intersects(buttonRect),
                                "update actions remain inside the card without overlapping status");
                    }
                    if (state == UpdateState::Downloading) {
                        require(progress->value() == 250 && progress->maximum() == 1000 &&
                                    progress->height() <= 8,
                                "download progress is accurate and uses a slim track");
                    }
                    if (locale == QStringLiteral("en_US")) {
                        scroll->verticalScrollBar()->setValue(0);
                        flushEvents();
                        snapshot(page, QStringLiteral("about-update-%1-%2-%3")
                                           .arg(appearance == styles::ThemeAppearance::Light
                                                    ? QStringLiteral("light")
                                                    : QStringLiteral("dark"))
                                           .arg(width)
                                           .arg(static_cast<int>(state)));
                    }
                }
            }
            QCoreApplication::removeTranslator(&translator);
        }
    }
    status = {UpdateState::Downloading, {}, {}, 0, 0};
    updates.statusChanged();
    require(progress->minimum() == 0 && progress->maximum() == 0,
            "downloads of unknown size use indeterminate progress");
    require(label->text() == QStringLiteral("Downloading update…"),
            "unknown download sizes do not display a misleading zero total");
    status = {UpdateState::Ready, QStringLiteral("1.2.3"),
              QStringLiteral("Close the recording first.")};
    updates.statusChanged();
    require(action->isEnabled() && label->text().contains(status.error) &&
                label->palette().color(QPalette::WindowText) ==
                    styles::ThemeManager::instance().themeColorScheme().map.colorWarningText,
            "blocked restarts keep their action and show the reason in the warning color");
    page.hide();
    styles::ThemeManager::instance().setThemeAppearance(styles::ThemeAppearance::Light);
}

void dividersFollowTheComponentLibraryAndUpdatesBreathe() {
    using adqt::widgets::AdButton;
    using adqt::widgets::AdDivider;
    QCoreApplication::setApplicationVersion(QStringLiteral(SNOW_SHOT_TEST_VERSION));
    snow_shot::update::UpdateService updates({});
    const_cast<snow_shot::update::UpdateStatus&>(updates.status()).state =
        snow_shot::update::UpdateState::Idle;
    AboutPageWidget page(nullptr, [](const QUrl&) { return true; }, &updates);
    page.resize(700, 540);
    page.show();
    flushEvents();
    const auto scheme = styles::ThemeManager::instance().themeColorScheme();
    const auto& metric = scheme.metricAlias;
    for (const char* name : {"aboutFeatureDivider", "aboutUpdateDivider", "aboutFooterDivider"}) {
        auto* divider = child<AdDivider>(page, name);
        require(divider->orientation() == AdDivider::Orientation::Horizontal &&
                    divider->dividerSize() == AdDivider::Size::Small,
                "About section dividers reuse the Ant Design divider component");
    }
    auto* updateDivider = child<AdDivider>(page, "aboutUpdateDivider");
    const auto dividerTokens = updateDivider->componentTokens();
    const auto dividerSemantics = updateDivider->semanticStyles();
    require(dividerTokens.colors.splitColor.has_value() &&
                dividerTokens.colors.splitColor.value() == scheme.map.colorSplit &&
                dividerSemantics.root.backgroundColor.has_value() &&
                dividerSemantics.root.backgroundColor.value() == QColor(Qt::transparent),
            "the in-card divider pins the theme split color and a transparent root so the "
            "version card stylesheet cannot reroute its palette");
    for (int i = 0; i < 6; ++i) {
        auto* separator =
            page.findChild<AdDivider*>(QStringLiteral("aboutFeatureSeparator%1").arg(i));
        require(separator != nullptr &&
                    separator->orientation() == AdDivider::Orientation::Vertical &&
                    separator->dividerSize() == AdDivider::Size::Small &&
                    separator->sizeHint().width() > 2,
                "feature separators reuse vertical Ant Design dividers with inline rail margins");
    }
    auto* panel = child<QFrame>(page, "aboutVersionPanel");
    const auto* panelLayout = qobject_cast<QVBoxLayout*>(panel->layout());
    require(panelLayout != nullptr && panelLayout->contentsMargins().bottom() >= metric.paddingXS,
            "the updates module keeps bottom breathing room inside the version card");
    auto* value = child<QLabel>(page, "aboutVersionValue");
    int rowBottom = value->mapTo(panel, QPoint()).y() + value->height();
    for (const char* name : {"aboutCopyVersion", "aboutReleaseNotes"}) {
        auto* button = child<AdButton>(page, name);
        rowBottom = std::max(rowBottom, button->mapTo(panel, QPoint()).y() + button->height());
    }
    const int railCenter =
        updateDivider->mapTo(panel, QPoint()).y() + (updateDivider->height() - 1) / 2;
    require(railCenter - rowBottom >= metric.paddingXS,
            "the updates module keeps clearance below the version number");
    page.hide();
}

void traySettingsAndFunctionNavigation() {
    const auto& registry = settings::builtInSettingsRegistry();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    const auto left = settings::SettingsSelectBinding::TrayLeftClickAction;
    const auto middle = settings::SettingsSelectBinding::TrayMiddleClickAction;
    require(backend.resetSection(settings::SettingsSectionReset::TrayBehavior),
            "reset tray settings");
    require(backend.selectValue(left).toString() == QStringLiteral("screenshot") &&
                backend.selectValue(middle).toString() == QStringLiteral("screenshot_fixed"),
            "tray reset must restore distinct defaults");
    for (const auto& action :
         {QStringLiteral("screenshot"), QStringLiteral("show_main_window"),
          QStringLiteral("screenshot_copy"), QStringLiteral("screenshot_fixed"),
          QStringLiteral("open_function_settings")}) {
        const auto previousMiddle = backend.selectValue(middle);
        require(backend.applySelectValue(left, action) && backend.selectValue(left) == action &&
                    backend.selectValue(middle) == previousMiddle,
                "left-click updates must preserve the middle-click choice");
        require(backend.applySelectValue(middle, action) && backend.selectValue(middle) == action &&
                    backend.selectValue(left) == action,
                "middle-click updates must preserve the left-click choice");
    }
    require(!backend.applySelectValue(middle, QStringLiteral("invalid")) &&
                backend.selectValue(middle).toString() == QStringLiteral("open_function_settings"),
            "invalid writes must preserve the last valid setting");
    require(backend.resetSection(settings::SettingsSectionReset::TrayBehavior) &&
                backend.selectValue(left).toString() == QStringLiteral("screenshot") &&
                backend.selectValue(middle).toString() == QStringLiteral("screenshot_fixed"),
            "reset must restore both modified tray settings");
    MainWindow window(registry, session);
    window.showInterfaceSettings();
    window.hide();
    window.showFunctionSettings();
    flushEvents();
    auto* card = window.findChild<ContentCardWidget*>();
    auto* sidebar = window.findChild<SidebarWidget*>();
    require(window.isVisible() && card != nullptr && sidebar != nullptr &&
                card->currentLocation().pageId == QStringLiteral("function-settings") &&
                card->currentLocation().sectionId == QStringLiteral("screenshot-settings") &&
                sidebar->currentRoute() == QStringLiteral("/settings/functionSettings"),
            "function settings action must show a hidden window and navigate from another page");
    window.hide();
}

void mainNavigationSearchThemesAndLanguages() {
    snow_shot::update::UpdateService updates({}, qApp);
    const_cast<snow_shot::update::UpdateStatus&>(updates.status()).state =
        snow_shot::update::UpdateState::Idle;
    QCoreApplication::setApplicationVersion(QStringLiteral(SNOW_SHOT_TEST_VERSION));
    const auto& registry = settings::builtInSettingsRegistry();
    require(registry.isValid(), "About preserves catalog validity");
    const auto* definition = registry.catalog().pageForRoute(QStringLiteral("/about"));
    require(definition != nullptr && definition->kind == settings::SettingsPageKind::About &&
                definition->sections.isEmpty(),
            "About is a dedicated page without settings sections");
    const auto resolved = registry.catalog().resolveLocation(
        {QStringLiteral("about"), QStringLiteral("stale-section"), QStringLiteral("stale-item")});
    require(resolved.pageId == QStringLiteral("about") && resolved.sectionId.isEmpty() &&
                resolved.itemId.isEmpty(),
            "About discards stale section and item locations");

    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    MainWindow window(registry, session);
    window.show();
    flushEvents();
    auto* sidebar = window.findChild<SidebarWidget*>();
    auto* card = window.findChild<ContentCardWidget*>();
    auto* header = window.findChild<MainContentHeaderWidget*>();
    require(sidebar != nullptr && card != nullptr && header != nullptr,
            "main interface components");
    auto* menu = sidebar->findChild<adqt::widgets::AdNavigationMenu*>();
    require(menu != nullptr, "main navigation menu");
    QModelIndex aboutIndex;
    for (int row = 0; row < menu->model()->rowCount(); ++row) {
        const auto index = menu->model()->index(row, 0);
        if (index.data(adqt::widgets::AdNavigationMenu::StableIdRole).toString() ==
            QStringLiteral("/about")) {
            aboutIndex = index;
            break;
        }
    }
    require(aboutIndex.isValid() && aboutIndex.data(Qt::DecorationRole).isValid(),
            "About has a top-level navigation item and icon");
    menu->activated(aboutIndex);
    flushEvents();
    auto* page = window.findChild<AboutPageWidget*>();
    require(page != nullptr && page->isVisible() &&
                card->currentRoute() == QStringLiteral("/about") &&
                sidebar->currentRoute() == QStringLiteral("/about"),
            "sidebar activation opens the About page in the main interface");
    require(card->currentSections().isEmpty() &&
                !child<adqt::widgets::AdTabs>(*header, "mainSectionTabs")->isVisible(),
            "About hides settings section tabs");

    const settings::SettingsSearchIndex search(registry);
    const auto results = search.search(QStringLiteral("version"));
    const auto aboutResult = std::find_if(results.cbegin(), results.cend(), [](const auto& result) {
        return result.location.pageId == QStringLiteral("about");
    });
    require(aboutResult != results.cend(), "version search finds About");
    QPointer<AboutPageWidget> previous(page);
    card->setCurrentRoute(QStringLiteral("/settings/generalSettings"));
    flushEvents();
    require(previous.isNull(), "leaving About releases its page and connections");
    header->locationRequested(aboutResult->location);
    flushEvents();
    page = window.findChild<AboutPageWidget*>();
    require(page != nullptr && sidebar->currentRoute() == QStringLiteral("/about"),
            "search navigation opens About and synchronizes the sidebar");

    QImage previousHero;
    QImage previousArtwork;
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        styles::ThemeManager::instance().setThemeAppearance(appearance);
        flushEvents();
        const auto scheme = styles::ThemeManager::instance().themeColorScheme();
        require(
            child<QLabel>(*page, "aboutVersionValue")->palette().color(QPalette::WindowText) ==
                    scheme.map.colorText &&
                child<QLabel>(*page, "aboutDescription")->palette().color(QPalette::WindowText) ==
                    scheme.map.colorTextSecondary,
            "About typography follows the active theme");
        require(child<QFrame>(*page, "aboutVersionPanel")
                    ->styleSheet()
                    .contains(scheme.map.colorBorderSecondary.name(QColor::HexArgb)),
                "compact version surface follows the neutral theme tokens");
        require(!child<QLabel>(*page, "aboutLogo")->pixmap().isNull(), "render the Snow Shot icon");
        auto* logo = child<QLabel>(*page, "aboutLogo");
        require(qFuzzyCompare(logo->pixmap().devicePixelRatio(), logo->devicePixelRatioF()),
                "the logo uses the current display pixel ratio");
        auto* artwork = child<QWidget>(*page, "aboutArtwork");
        const QImage hero = child<QWidget>(*page, "aboutHero")->grab().toImage();
        const QImage renderedArtwork = artwork->grab().toImage();
        require(!renderedArtwork.isNull() && !artwork->accessibleName().isEmpty(),
                "the embedded artwork is rendered and has an accessible name");
        if (!previousHero.isNull()) {
            require(hero != previousHero && renderedArtwork != previousArtwork,
                    "both the hero surface and artwork adapt to dark mode");
        }
        previousHero = hero;
        previousArtwork = renderedArtwork;
        snapshot(window, appearance == styles::ThemeAppearance::Light
                             ? QStringLiteral("about-light")
                             : QStringLiteral("about-dark"));
    }

    styles::ThemeManager::instance().setThemeAppearance(styles::ThemeAppearance::Light);
    for (const QString& locale :
         {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        QTranslator translator;
        require(translator.load(QStringLiteral(SNOW_SHOT_TEST_TRANSLATIONS_DIR) +
                                QStringLiteral("/snow_shot_%1.qm").arg(locale)),
                "load a compiled application translation catalog");
        QCoreApplication::installTranslator(&translator);
        flushEvents();
        const QString translatedTitle = translator.translate("AboutPageWidget", "About Snow Shot");
        require(!translatedTitle.isEmpty() && page->accessibleName() == translatedTitle,
                "an open About page retranslates immediately");
        require(child<QLabel>(*page, "aboutVersionValue")->text() ==
                    QStringLiteral(SNOW_SHOT_TEST_VERSION),
                "language changes preserve the release version");
        require(child<adqt::widgets::AdButton>(*page, "aboutCopyVersion")->text() ==
                    translator.translate("AboutPageWidget", "Copy version number"),
                "copy action retranslates");
        require(child<QLabel>(*page, "aboutLicense")
                    ->text()
                    .contains(translator.translate("AboutPageWidget",
                                                   "GNU General Public License v3.0 or later")),
                "license details retranslate");
        require(
            child<QLabel>(*page, "aboutOpenSource")->text() ==
                    translator.translate("AboutPageWidget", "Free · Open source") &&
                child<QLabel>(*page, "aboutDescription")->text() ==
                    translator.translate(
                        "AboutPageWidget",
                        "Capture, annotate, recognize text, and record your screen,\n"
                        "so every moment on screen can be expressed clearly and shared easily."),
            "the new hero copy retranslates immediately");
        require(child<QLabel>(*page, "aboutTagline")
                    ->text()
                    .contains(translator.translate("AboutPageWidget", "Elegant screenshots")),
                "the two-tone headline retranslates");
        require(child<QLabel>(*page, "aboutSlogan")->text() ==
                    translator.translate("AboutPageWidget", "Snow Shot · Make expression clearer"),
                "the footer slogan retranslates");
        const std::array<const char*, 6> features{"Screenshot capture", "Easy annotation",
                                                  "Text recognition",   "Screen recording",
                                                  "Pin to screen",      "Screenshot history"};
        for (size_t i = 0; i < features.size(); ++i) {
            auto* label = page->findChild<QLabel*>(QStringLiteral("aboutFeatureLabel%1").arg(i));
            require(label != nullptr &&
                        label->text() == translator.translate("AboutPageWidget", features[i]),
                    "every feature label retranslates");
            auto* icon = page->findChild<QLabel*>(QStringLiteral("aboutFeatureIcon%1").arg(i));
            require(icon != nullptr && !icon->pixmap().isNull(), "every feature icon renders");
        }
        require(child<QAbstractButton>(*page, "aboutWebsite")->accessibleName() ==
                        translator.translate("AboutPageWidget", "Official website") &&
                    child<QWidget>(*page, "aboutArtwork")->accessibleName() ==
                        translator.translate(
                            "AboutPageWidget",
                            "Screenshot selection, annotation tools, and recognized text"),
                "resource and illustration accessibility copy follows the active language");
        require(sidebar->currentRoute() == QStringLiteral("/about"),
                "translated navigation preserves About selection");
        auto* scroll = page->findChild<adqt::widgets::AdScrollArea*>();
        snapshot(window, QStringLiteral("about-%1").arg(locale));
        if (scroll != nullptr && scroll->verticalScrollBar()->maximum() != 0) {
            std::cerr << "Default About viewport " << scroll->viewport()->width() << 'x'
                      << scroll->viewport()->height() << ", content height "
                      << scroll->widget()->height() << '\n';
        }
        require(scroll != nullptr && scroll->verticalScrollBar()->maximum() == 0,
                "all About information fits in the default window size");
        window.resize(512, 316);
        sidebar->setCollapsed(true);
        flushEvents();
        require(scroll != nullptr && scroll->horizontalScrollBar()->maximum() == 0,
                "narrow About page fits without horizontal scrolling");
        require(scroll->verticalScrollBar()->maximum() > 0,
                "short windows can scroll to all About content");
        snapshot(window, QStringLiteral("about-%1-compact").arg(locale));
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        flushEvents();
        snapshot(window, QStringLiteral("about-%1-compact-bottom").arg(locale));
        window.resize(900, 556);
        sidebar->setCollapsed(false);
        QCoreApplication::removeTranslator(&translator);
        flushEvents();
    }
    window.hide();
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    // Static Qt's offscreen font database does not discover Windows fonts.
    const QDir fonts(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts"));
    for (const QString& file : {QStringLiteral("segoeui.ttf"), QStringLiteral("seguisb.ttf"),
                                QStringLiteral("msyh.ttc"), QStringLiteral("msyhbd.ttc")}) {
        if (QFileInfo::exists(fonts.filePath(file))) {
            QFontDatabase::addApplicationFont(fonts.filePath(file));
        }
    }
#endif
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("about_page_tests"));
    QTemporaryDir directory;
    require(directory.isValid(), "isolated About test storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 8000}).success,
            "initialize isolated storage");
    styles::ThemeManager::instance().initialize(application);
    versionIsExactSelectableAndCopyable();
    absentVersionDoesNotInventARelease();
    stableVersionsDoNotClaimToBePreviews();
    projectLinkSurfacesMatchStandardButtons();
    projectLinksAreExplicitAccessibleAndRecoverable();
    updatePolicyAndUnavailableCopy();
    updateStatesFitTheVersionPanel();
    dividersFollowTheComponentLibraryAndUpdatesBreathe();
    traySettingsAndFunctionNavigation();
    mainNavigationSearchThemesAndLanguages();
    largerTypeKeepsEveryActionReachable();
    storage.shutdown();
    return 0;
}
