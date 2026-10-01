#include "window_close_shortcut_test_support.h"
#include "snow_shot/presentation/components/actionrow.h"
#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/maincontentheaderwidget.h"
#include "snow_shot/presentation/components/sidebarwidget.h"
#include "snow_shot/presentation/components/titlebarwidget.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "icon_renderer.h"
#include <QPainter>
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/mainwindowskincontroller.h"
#include "snow_shot/presentation/mainwindowskinwidget.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/message.h"
#include "widgets/navigation_menu.h"
#include "widgets/scroll_area.h"
#include "widgets/select.h"
#include "snowimageqtcodec.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QImage>
#include <QElapsedTimer>
#include <QFile>
#include <QLinearGradient>
#include <QPixmap>
#include <QFontDatabase>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPalette>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <functional>

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
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

void requireSmoothTitles(QWidget& container, int expectedSize) {
    int titleCount = 0;
    for (const auto* label : container.findChildren<QLabel*>()) {
        if (label->text().isEmpty() || label->font().pixelSize() != expectedSize) {
            continue;
        }
        ++titleCount;
        require(label->font().hintingPreference() == QFont::PreferNoHinting,
                "large titles must inherit the main window's unhinted outline rendering");
    }
    require(titleCount > 0, "exercise actual large title labels, not just the window font");
}

void customTitleBarUsesPlatformWindowControls() {
    const auto scheme = styles::ThemeManager::instance().themeColorScheme();
    QWidget host;
    host.resize(360, 80);
    host.setWindowTitle(QStringLiteral("SnowShot"));
    TitleBarWidget titleBar(scheme.metricAlias, &host);
    titleBar.resize(host.width(), titleBar.height());
    host.show();
    flushEvents();

    auto* closeButton = titleBar.findChild<QAbstractButton*>(QStringLiteral("closeWindowButton"));
    auto* minimizeButton =
        titleBar.findChild<QAbstractButton*>(QStringLiteral("minimizeWindowButton"));
    auto* maximizeButton =
        titleBar.findChild<QAbstractButton*>(QStringLiteral("maximizeWindowButton"));
#ifdef Q_OS_MACOS
    require(closeButton == nullptr && minimizeButton == nullptr && maximizeButton == nullptr,
            "macOS must leave traffic-light rendering and interaction to AppKit");
#else
    require(closeButton != nullptr && minimizeButton != nullptr && maximizeButton != nullptr,
            "the custom title bar must own close, minimize, and maximize controls");
    require(minimizeButton->x() < maximizeButton->x() && maximizeButton->x() < closeButton->x(),
            "caption buttons must keep the Windows minimize/maximize/close order");
    require(minimizeButton->x() >= titleBar.width() / 2,
            "Windows caption buttons must stay right-aligned in the custom title bar");

#ifdef Q_OS_WIN
    require(titleBar.height() == 32, "Windows caption must use the standard 32 DIP height");
    auto* icon = titleBar.findChild<QLabel*>(QStringLiteral("windowSystemMenuIcon"));
    require(icon != nullptr && icon->geometry() == QRect(16, 8, 16, 16),
            "the 16 DIP app icon must have the standard leading inset and vertical alignment");
    require(!icon->pixmap().isNull(), "the caption must render the actual application icon");
    const QImage captionIcon = icon->pixmap().toImage();
    bool paintsWhiteBackdrop = false;
    for (int y = 0; y < captionIcon.height() && !paintsWhiteBackdrop; ++y) {
        for (int x = 0; x < captionIcon.width(); ++x) {
            const QColor pixel = captionIcon.pixelColor(x, y);
            if (pixel.alpha() >= 250 && pixel.red() >= 240 && pixel.green() >= 240 &&
                pixel.blue() >= 240) {
                paintsWhiteBackdrop = true;
                break;
            }
        }
    }
    require(!paintsWhiteBackdrop,
            "the caption icon must drop the application icon's white background");
    for (const auto* button : {minimizeButton, maximizeButton, closeButton}) {
        require(button->size() == QSize(46, 32) && button->y() == 0,
                "caption buttons must provide full-height 46 DIP targets");
    }
    require(closeButton->geometry().right() == titleBar.width() - 1 &&
                minimizeButton->geometry().right() + 1 == maximizeButton->x() &&
                maximizeButton->geometry().right() + 1 == closeButton->x(),
            "caption buttons must touch each other and the right window edge");
    const QImage maximizeImage = maximizeButton->grab().toImage();
#endif
    const QString maximizeText = maximizeButton->accessibleName();
    titleBar.setMaximized(true);
    require(!maximizeButton->accessibleName().isEmpty() &&
                maximizeButton->accessibleName() != maximizeText,
            "the maximize control must expose its restore action while maximized");
#ifdef Q_OS_WIN
    require(maximizeButton->grab().toImage() != maximizeImage,
            "maximizing must change the visible glyph to overlapping restore windows");
#endif
    titleBar.setMaximized(false);
    require(maximizeButton->accessibleName() == maximizeText,
            "the maximize control must restore its maximize action in the normal state");
#ifdef Q_OS_WIN
    require(maximizeButton->grab().toImage() == maximizeImage,
            "restoring must recover the original maximize glyph");
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        styles::ThemeManager::instance().setThemeAppearance(appearance);
        flushEvents();
        closeButton->setDown(true);
        require(closeButton->grab().toImage().pixelColor(0, 0) == QColor(196, 43, 28),
                "close pressed state must fill the entire button red in both themes");
        closeButton->setDown(false);
        const QImage minimize = minimizeButton->grab().toImage();
        const QColor buttonBackground = minimize.pixelColor(0, 0);
        const auto currentScheme = styles::ThemeManager::instance().themeColorScheme();
        const QColor ink = host.isActiveWindow() ? currentScheme.map.colorText
                                                 : currentScheme.map.colorTextTertiary;
        QImage solid(1, 1, QImage::Format_ARGB32_Premultiplied);
        solid.fill(buttonBackground);
        {
            QPainter painter(&solid);
            painter.fillRect(solid.rect(), ink);
        }
        const QColor solidInk = solid.pixelColor(0, 0);
        // The straight minimize stroke must consist entirely of solid physical
        // pixels, even when the window is rendered at a fractional display scale.
        for (int y = 0; y < minimize.height(); ++y) {
            for (int x = 0; x < minimize.width(); ++x) {
                const QColor pixel = minimize.pixelColor(x, y);
                require(pixel == buttonBackground || pixel == solidInk,
                        "caption strokes must not acquire blurred fractional-pixel edges");
            }
        }
        const auto normal = titleBar.grab().toImage();
        const qreal scale = normal.devicePixelRatio();
        const QColor background = titleBar.palette().color(QPalette::Window);
        adqt::icons::IconRenderRequest wordmarkRequest;
        const int logoHeight = std::clamp(scheme.metricAlias.fontSizeSM, 10, 14);
        wordmarkRequest.logicalSize = QSize(qRound(logoHeight * 95.0 / 17.0), logoHeight);
        wordmarkRequest.devicePixelRatio = scale;
        const auto wordmark = adqt::icons::renderIconPixmap(
            snow_shot::presentation::icons::custom::brand::SnowShotLogo(
                adqt::icons::IconColors::primary(ink)),
            wordmarkRequest);
        QImage expected(wordmark.size(), QImage::Format_ARGB32_Premultiplied);
        expected.setDevicePixelRatio(scale);
        expected.fill(background);
        {
            QPainter painter(&expected);
            painter.drawPixmap(0, 0, wordmark);
        }
        const QRect wordmarkRect(qRound(48 * scale),
                                 qRound((titleBar.height() * scale - wordmark.height()) / 2.0),
                                 wordmark.width(), wordmark.height());
        require(normal.copy(wordmarkRect).convertToFormat(expected.format()) == expected,
                "the title must preserve the original SVG wordmark artwork exactly");
        bool hasCaptionText = false;
        for (int y = 8; y < 24; ++y) {
            for (int x = 48; x < 110; ++x) {
                hasCaptionText |=
                    normal.pixelColor(qRound(x * scale), qRound(y * scale)) != background;
            }
        }
        require(hasCaptionText, "the original wordmark must be rendered next to the left icon");
        const QString renderDir = qEnvironmentVariable("SNOW_TITLEBAR_RENDER_DIR");
        if (!renderDir.isEmpty()) {
            QDir().mkpath(renderDir);
            require(
                normal.save(QDir(renderDir).filePath(appearance == styles::ThemeAppearance::Light
                                                         ? QStringLiteral("titlebar-light.png")
                                                         : QStringLiteral("titlebar-dark.png"))),
                "save requested title-bar visual review images");
        }
    }
    host.setWindowTitle(QString(200, QLatin1Char('W')));
    titleBar.resize(240, titleBar.height());
    flushEvents();
    require(closeButton->geometry().right() == titleBar.width() - 1 &&
                minimizeButton->x() == titleBar.width() - 138,
            "a long caption must not displace or shrink the window controls");
    titleBar.grab();
#endif
#endif
}

void titleBarBackgroundMatchesNavigationMenu() {
    auto& themeManager = styles::ThemeManager::instance();
    themeManager.setThemeAppearance(styles::ThemeAppearance::Light);

    QWidget host;
    host.resize(360, 80);
    TitleBarWidget titleBar(themeManager.themeColorScheme().metricAlias, &host);
    titleBar.resize(host.width(), titleBar.height());
    host.show();
    flushEvents();

    const auto lightMenuColors = adqt::widgets::AdNavigationMenu::resolveColorTokens(&titleBar);
    require(titleBar.autoFillBackground() &&
                titleBar.palette().color(QPalette::Window) == lightMenuColors.itemBackground &&
                lightMenuColors.itemBackground ==
                    themeManager.themeColorScheme().map.colorBgContainer,
            "the light title bar must use the navigation menu item background");

    themeManager.setThemeAppearance(styles::ThemeAppearance::Dark);
    flushEvents();
    const auto darkMenuColors = adqt::widgets::AdNavigationMenu::resolveColorTokens(&titleBar);
    require(titleBar.palette().color(QPalette::Window) == darkMenuColors.itemBackground &&
                darkMenuColors.itemBackground !=
                    themeManager.themeColorScheme().map.colorBgContainer,
            "the dark title bar must use the navigation menu item background");

    themeManager.setThemeAppearance(styles::ThemeAppearance::Light);
    flushEvents();
    require(titleBar.palette().color(QPalette::Window) ==
                adqt::widgets::AdNavigationMenu::resolveColorTokens(&titleBar).itemBackground,
            "returning to the light theme must restore the navigation menu item background");
}

void skinMasksCompositeOnceAndSurviveThemeChanges() {
    const auto& registry = settings::builtInSettingsRegistry();
    auto& themeManager = styles::ThemeManager::instance();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    const QColor base(40, 80, 160);
    QWidget host;
    host.resize(900, 560);
    QPalette hostPalette = host.palette();
    hostPalette.setColor(QPalette::Window, base);
    host.setPalette(hostPalette);
    host.setAutoFillBackground(true);
    TitleBarWidget titleBar(themeManager.themeColorScheme().metricAlias, &host);
    titleBar.resize(host.width(), titleBar.height());
    SidebarWidget sidebar(registry, &host);
    sidebar.setGeometry(0, 50, 220, 500);
    MainContentHeaderWidget header(registry, themeManager.themeColorScheme().metricAlias, &host);
    header.setGeometry(230, 50, 650, 90);
    ContentCardWidget card(registry, session, &host);
    card.setGeometry(230, 150, 650, 400);
    host.show();
    flushEvents();

    const auto composite = [&base](QColor mask, qreal opacity) {
        QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
        image.fill(base);
        mask.setAlphaF(mask.alphaF() * static_cast<float>(opacity));
        QPainter painter(&image);
        painter.fillRect(image.rect(), mask);
        painter.end();
        return image.pixelColor(0, 0);
    };
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        // Set the mask before changing theme so the theme event must preserve it.
        titleBar.setSkinMaskOpacity(0.5);
        sidebar.setSkinMaskOpacity(0.5);
        header.setSkinMaskOpacity(0.5);
        card.setSkinMaskOpacity(0.5);
        themeManager.setThemeAppearance(appearance);
        flushEvents();
        const QColor navigation =
            adqt::widgets::AdNavigationMenu::resolveColorTokens(&sidebar).itemBackground;
        const QColor container = themeManager.themeColorScheme().map.colorBgContainer;
        for (const qreal opacity : {0.5, 0.0, 1.0}) {
            titleBar.setSkinMaskOpacity(opacity);
            sidebar.setSkinMaskOpacity(opacity);
            header.setSkinMaskOpacity(opacity);
            card.setSkinMaskOpacity(opacity);
            flushEvents();
            const QImage rendered = host.grab().toImage();
            const qreal scale = rendered.devicePixelRatio();
            const auto sample = [&rendered, scale](const QWidget& widget, const QPoint& point) {
                const QPoint hostPoint = widget.pos() + point;
                return rendered.pixelColor(qRound(hostPoint.x() * scale),
                                           qRound(hostPoint.y() * scale));
            };
            require(sample(titleBar, QPoint(2, 2)) == composite(navigation, opacity),
                    "the title-bar mask must blend the theme surface once over the skin");
            require(sample(sidebar, QPoint(2, 2)) == composite(navigation, opacity),
                    "navigation menu and scroll viewport must not duplicate the sidebar mask");
            require(sample(sidebar, QPoint(2, sidebar.height() - 10)) ==
                        composite(navigation, opacity),
                    "the collapse trigger must share the sidebar's single mask");
            require(sample(header, QPoint(2, 2)) == composite(container, opacity),
                    "the header must retain its mask through theme changes");
            require(sample(card, QPoint(8, 40)) == composite(container, opacity),
                    "the page card must blend one container mask over the skin");
            require(sidebar.palette().color(QPalette::Base).alpha() == 255 &&
                        titleBar.palette().color(QPalette::Base).alpha() == 255 &&
                        header.palette().color(QPalette::Base).alpha() == 255,
                    "skin masks must not make inherited control base colors translucent");
        }
        require(sidebar.findChild<adqt::widgets::AdNavigationMenu*>()
                        ->resolvedColorTokens()
                        .itemBackground == navigation,
                "clearing a skin mask must restore the original navigation tokens");
    }
    themeManager.setThemeAppearance(styles::ThemeAppearance::Light);
}

void mainWindowTitlesKeepSmoothRendering() {
    const QFont applicationFont = QApplication::font();
    const auto& registry = settings::builtInSettingsRegistry();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    MainWindow window(registry, session);
    window.show();
    flushEvents();
#ifdef Q_OS_MACOS
    auto* titleBar = window.findChild<TitleBarWidget*>();
    require(titleBar != nullptr,
            "macOS must place native traffic lights over SnowShot's in-content title bar");
    require(window.windowFlags().testFlag(Qt::ExpandedClientAreaHint) &&
                window.windowFlags().testFlag(Qt::NoTitleBarBackgroundHint) &&
                window.testAttribute(Qt::WA_LayoutOnEntireRect),
            "macOS must expand SnowShot's layout into the transparent native title-bar area");
    require(window.findChild<QWidget*>(QStringLiteral("titleBarBottomShadow")) != nullptr,
            "macOS must retain SnowShot's custom title-bar separator shadow");
    require(titleBar->closeButton() == nullptr && titleBar->minimizeButton() == nullptr &&
                titleBar->maximizeButton() == nullptr,
            "macOS must not render duplicate custom traffic-light controls");
#else
    auto* titleBar = window.findChild<TitleBarWidget*>();
    require(titleBar != nullptr, "non-macOS windows must retain the existing custom title bar");
    require(titleBar->maximizeButton() != nullptr,
            "the custom main-window title bar must expose a maximize control");
    titleBar->maximizeButton()->click();
    flushEvents();
    require(window.isMaximized(), "the maximize title-bar control must maximize the main window");
    titleBar->maximizeButton()->click();
    flushEvents();
    require(!window.isMaximized(), "the maximize title-bar control must restore the main window");
#endif
    auto* card = window.findChild<ContentCardWidget*>();
    require(card != nullptr, "main window content exists");
    const QString shortcutRoute = card->currentRoute();
    require(!card->findChildren<ActionRow*>().isEmpty(), "default route contains shortcut buttons");

    for (auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        styles::ThemeManager::instance().setThemeAppearance(appearance);
        for (bool settingsPage : {false, true, false}) {
            if (settingsPage) {
                window.showInterfaceSettings();
            } else {
                card->setCurrentRoute(shortcutRoute);
            }
            flushEvents();
            const int titleSize =
                styles::ThemeManager::instance().themeColorScheme().metricAlias.fontSizeLG;
            requireSmoothTitles(*card, titleSize);
            QEvent languageChange(QEvent::LanguageChange);
            QApplication::sendEvent(&window, &languageChange);
            QEvent dpiChange(QEvent::DevicePixelRatioChange);
            QApplication::sendEvent(&window, &dpiChange);
            flushEvents();
            requireSmoothTitles(*card, titleSize);
        }
    }
    require(QApplication::font() == applicationFont,
            "main window typography must not change the application font for other windows");
    const QString family = QStringLiteral("SnowShot UI Font Test Family");
    require(styles::ThemeManager::instance().setAppFontFamily(family),
            "apply a new interface family");
    flushEvents();
    require(window.font().family() == family, "existing main window follows app font changes");
    for (const auto* label : card->findChildren<QLabel*>()) {
        require(label->font().family() == family,
                "existing main window labels follow app font changes");
    }
    require(styles::ThemeManager::instance().setAppFontFamily(QString()), "restore system font");
    flushEvents();
    require(window.font().family() == applicationFont.family(),
            "main window restores the platform family");
}

void mainWindowSkinIsContinuousAndRestoresTheme(const QString& previewDirectory) {
    namespace presentation = snow_shot::presentation;
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    auto& themeManager = styles::ThemeManager::instance();
    const auto previousAppearance = themeManager.themeColorScheme().appearance;
    const auto snapshot = configuration.snapshot();
    const auto savedTranslationPage =
        snapshot.value(QStringLiteral("extended_features/translation_page_enabled"));
    QMap<QString, QJsonValue> savedSkin;
    for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it) {
        if (it.key().startsWith(QStringLiteral("interface/skin_"))) {
            savedSkin.insert(it.key(), it.value());
        }
    }
    QTemporaryDir fixtureDirectory;
    require(fixtureDirectory.isValid(), "create the main-window skin fixture directory");
    QImage source(1200, 700, QImage::Format_ARGB32_Premultiplied);
    {
        QPainter painter(&source);
        QLinearGradient gradient(QPointF(0, 0), QPointF(source.width(), source.height()));
        gradient.setColorAt(0.0, QColor(245, 196, 164));
        gradient.setColorAt(0.45, QColor(112, 161, 209));
        gradient.setColorAt(1.0, QColor(73, 56, 117));
        painter.fillRect(source.rect(), gradient);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(237, 218, 187));
        painter.drawEllipse(QRectF(890, 80, 190, 190));
        painter.setBrush(QColor(177, 218, 231));
        painter.drawRoundedRect(QRectF(110, 110, 150, 450), 75, 75);
    }
    const QString fixturePath = fixtureDirectory.filePath(QStringLiteral("skin.png"));
    QFile fixture(fixturePath);
    require(fixture.open(QIODevice::WriteOnly), "open the main-window skin fixture");
    const QByteArray encoded = snow_shot::image_codec::encodePng(source);
    require(!encoded.isEmpty() && fixture.write(encoded) == encoded.size(),
            "write the main-window skin fixture");
    fixture.close();

    const auto& registry = settings::builtInSettingsRegistry();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    MainWindow window(registry, session);
    window.showFunctionSettings();
    flushEvents();
    // Offscreen screens can constrain the initial show at fractional DPI.
    // Resize the visible window so these checks use the real 900 by 640 layout.
    window.resize(900, 640);
    flushEvents();
    auto* root = window.centralWidget();
    auto* titleBar = window.findChild<TitleBarWidget*>();
    auto* sidebar = window.findChild<SidebarWidget*>();
    auto* header = window.findChild<MainContentHeaderWidget*>();
    auto* card = window.findChild<ContentCardWidget*>();
    require(root && titleBar && sidebar && header && card,
            "exercise the actual main-window skin and surface hierarchy");
    require(root->metaObject() == &QWidget::staticMetaObject && root->autoFillBackground() &&
                window.findChild<presentation::MainWindowSkinWidget*>() == nullptr &&
                presentation::MainWindowSkinController::existingInstance() == nullptr,
            "an unset skin must use the original QWidget without any skin runtime or hooks");
    for (int step = 0; step < 12; ++step) {
        window.resize(900 + step, 640 + step);
        QEvent dprChange(QEvent::DevicePixelRatioChange);
        QApplication::sendEvent(root, &dprChange);
        flushEvents();
    }
    window.resize(900, 640);
    flushEvents();
    require(presentation::MainWindowSkinController::existingInstance() == nullptr &&
                window.findChild<presentation::MainWindowSkinWidget*>() == nullptr,
            "ordinary no-skin resize, DPI and paint events must never create a skin runtime");
    const QImage original = root->grab().toImage();
    const auto waitUntil = [](const std::function<bool()>& ready) {
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            flushEvents();
            if (ready()) {
                return;
            }
            require(timer.elapsed() < 15000, "main-window skin preparation must complete");
            QThread::msleep(1);
        }
    };
    require(configuration.setValues(
                {{QStringLiteral("interface/skin_path"), fixturePath},
                 {QStringLiteral("interface/skin_display_mode"), QStringLiteral("overlay")},
                 {QStringLiteral("interface/skin_opacity"), 100},
                 {QStringLiteral("interface/skin_blur_level"), 0},
                 {QStringLiteral("interface/skin_mask_opacity"), 0}}),
            "configure the main-window skin fixture");
    waitUntil([&] {
        auto* skin = window.findChild<presentation::MainWindowSkinWidget*>();
        auto* controller = presentation::MainWindowSkinController::existingInstance();
        return skin && skin->skinActive() && controller && !controller->diagnostics().busy;
    });
    QPointer<presentation::MainWindowSkinController> controller =
        presentation::MainWindowSkinController::existingInstance();
    struct SurfaceSample {
        QWidget* surface;
        QPoint point;
        bool navigation;
    };
    const SurfaceSample samples[] = {{titleBar, QPoint(2, 2), true},
                                     {sidebar, QPoint(2, 10), true},
                                     {sidebar, QPoint(2, sidebar->height() - 10), true},
                                     {header, QPoint(2, 10), false},
                                     {card, QPoint(8, 40), false}};
    const auto sample = [root](const QImage& image, const QWidget& surface, const QPoint& point) {
        const QPoint position = surface.mapTo(root, point);
        const qreal scale = image.devicePixelRatio();
        return image.pixelColor(qRound(position.x() * scale), qRound(position.y() * scale));
    };
    const auto closeColor = [](const QColor& actual, const QColor& expected) {
        return std::abs(actual.red() - expected.red()) <= 2 &&
               std::abs(actual.green() - expected.green()) <= 2 &&
               std::abs(actual.blue() - expected.blue()) <= 2 && actual.alpha() == expected.alpha();
    };
    const auto reference =
        presentation::prepareMainWindowSkin(source, root->size(), root->devicePixelRatioF(),
                                            presentation::MainWindowSkinDisplayMode::Overlay, 0);
    require(!reference.image.isNull(), "prepare the continuous main-window reference image");
    const auto paintCounts = controller->diagnostics();
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        themeManager.setThemeAppearance(appearance);
        for (const int mask : {0, 50, 100}) {
            require(configuration.setValue(QStringLiteral("interface/skin_mask_opacity"), mask),
                    "adjust the actual main-window mask");
            waitUntil([&] {
                return !controller->diagnostics().busy && controller->maskOpacity() == mask / 100.0;
            });
            const QImage rendered = root->grab().toImage();
            QImage skinOnly(rendered.size(), QImage::Format_ARGB32_Premultiplied);
            skinOnly.setDevicePixelRatio(rendered.devicePixelRatio());
            {
                QPainter painter(&skinOnly);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.fillRect(root->rect(), themeManager.themeColorScheme().map.colorBgLayout);
                painter.drawImage(QRectF(root->rect()), reference.image,
                                  QRectF(reference.image.rect()));
            }
            for (const auto& surface : samples) {
                QColor expected = sample(skinOnly, *surface.surface, surface.point);
                QImage composite(1, 1, QImage::Format_ARGB32_Premultiplied);
                composite.fill(expected);
                QColor tint =
                    surface.navigation
                        ? adqt::widgets::AdNavigationMenu::resolveColorTokens(root).itemBackground
                        : themeManager.themeColorScheme().map.colorBgContainer;
                tint.setAlphaF(tint.alphaF() * static_cast<float>(mask / 100.0));
                {
                    QPainter painter(&composite);
                    painter.fillRect(composite.rect(), tint);
                }
                expected = composite.pixelColor(0, 0);
                const QColor actual = sample(rendered, *surface.surface, surface.point);
                if (!closeColor(actual, expected)) {
                    const QPoint position = surface.surface->mapTo(root, surface.point);
                    std::cerr << "skin surface " << surface.surface->metaObject()->className()
                              << " at root " << position.x() << ',' << position.y() << " mask "
                              << mask << " theme "
                              << (appearance == styles::ThemeAppearance::Light ? "light" : "dark")
                              << " actual " << actual.name(QColor::HexArgb).toStdString()
                              << " expected " << expected.name(QColor::HexArgb).toStdString()
                              << " root " << root->width() << 'x' << root->height() << " raster "
                              << rendered.width() << 'x' << rendered.height() << " DPR "
                              << rendered.devicePixelRatio() << " frame "
                              << controller->frame().image.width() << 'x'
                              << controller->frame().image.height() << '\n';
                    const QString diagnosticDirectory =
                        previewDirectory.isEmpty()
                            ? QDir::current().absoluteFilePath(QStringLiteral("skin-diagnostics"))
                            : previewDirectory;
                    require(QDir().mkpath(diagnosticDirectory), "create skin diagnostics folder");
                    require(
                        rendered.save(QDir(diagnosticDirectory)
                                          .filePath(QStringLiteral("skin-failure-actual.png"))) &&
                            skinOnly.save(
                                QDir(diagnosticDirectory)
                                    .filePath(QStringLiteral("skin-failure-background.png"))),
                        "save skin compositing failure diagnostics");
                }
                require(closeColor(actual, expected),
                        "title, navigation, header and pages must share one continuous skin image "
                        "with one mask per surface");
            }
            require(closeColor(sample(rendered, *card, QPoint(-4, 40)),
                               sample(skinOnly, *card, QPoint(-4, 40))),
                    "structural content fills must expose skin gaps even at 100 percent mask");
        }
    }
    require(configuration.setValue(QStringLiteral("interface/skin_mask_opacity"), 0),
            "expose the skin for scrolling alignment verification");
    waitUntil([&] { return !controller->diagnostics().busy && controller->maskOpacity() == 0.0; });
    auto* pageScroll = card->findChild<adqt::widgets::AdScrollArea*>(settings::generatedObjectName(
        QStringLiteral("settings-scroll"), QStringLiteral("function-settings")));
    require(pageScroll && pageScroll->verticalScrollBar()->maximum() > 0,
            "the actual function settings page must provide real scrolling content");
    const QImage beforeScroll = root->grab().toImage();
    QScrollBar* scrollBar = pageScroll->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
    waitUntil([&] { return scrollBar->value() > 0; });
    const QImage afterScroll = root->grab().toImage();
    for (const int y : {40, card->height() / 2, card->height() - 40}) {
        const QPoint position(8, y);
        require(pageScroll->viewport()->rect().contains(
                    pageScroll->viewport()->mapFrom(card, position)),
                "scrolling alignment samples must be inside the scrolling page viewport");
        require(
            closeColor(sample(afterScroll, *card, position), sample(beforeScroll, *card, position)),
            "scrolling page content must leave the skin fixed in main-window coordinates");
    }
    scrollBar->setValue(scrollBar->minimum());
    waitUntil([&] { return scrollBar->value() == scrollBar->minimum(); });
    require(controller->diagnostics().decodeJobs == paintCounts.decodeJobs &&
                controller->diagnostics().preparationJobs == paintCounts.preparationJobs,
            "theme, mask and scroll changes in the actual main window must reuse the raster");

    // Exercise actual generated controls and the persistent header selector. Render
    // their own paint into transparent images so parent card masks cannot disguise
    // an opaque control fill or whole-widget opacity that also fades its text.
    // Keep unchanged shadows and border antialiasing in the zero-mask reference.
    const auto controlImage = [](QWidget& control) {
        QImage image(control.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        control.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
        return image;
    };
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        themeManager.setThemeAppearance(appearance);
        window.showSettingsLocation(QStringLiteral("interface-settings"), QStringLiteral("skin"));
        require(configuration.setValue(QStringLiteral("interface/skin_mask_opacity"), 100),
                "restore control backgrounds before taking their reference");
        waitUntil([&] { return controller->maskOpacity() == 1.0; });
        auto* select = card->findChild<adqt::widgets::AdSelect*>(settings::generatedObjectName(
            QStringLiteral("settings-control"), QStringLiteral("interface.skin.display-mode")));
        auto* button =
            card->findChild<adqt::widgets::AdButton*>(QStringLiteral("pathInputBrowseButton"));
        auto* search = header->findChild<adqt::widgets::AdSelect*>();
        require(select && button && search, "review real settings buttons, selects and search");
        const QList<QWidget*> controls{select, button, search};
        QList<QImage> references;
        for (auto* control : controls) {
            references.push_back(controlImage(*control));
        }
        require(configuration.setValue(QStringLiteral("interface/skin_mask_opacity"), 0),
                "capture control shadows and ink without background fills");
        waitUntil([&] { return controller->maskOpacity() == 0.0; });
        QList<QImage> zeroReferences;
        for (auto* control : controls) {
            zeroReferences.push_back(controlImage(*control));
        }
        for (const int mask : {50, 0, 100}) {
            require(configuration.setValue(QStringLiteral("interface/skin_mask_opacity"), mask),
                    "change mask with existing controls still alive");
            waitUntil([&] { return controller->maskOpacity() == mask / 100.0; });
            for (qsizetype index = 0; index < controls.size(); ++index) {
                const QImage actual = controlImage(*controls[index]);
                const QPoint fill(8, 4);
                const QColor referenceColor = references[index].pixelColor(fill);
                const QColor zeroColor = zeroReferences[index].pixelColor(fill);
                require(referenceColor.alpha() > 0, "sample an actual painted control fill");
                require(
                    referenceColor != zeroColor,
                    "zero mask must expose the control's background while retaining its shadow");
                const int expectedAlpha =
                    qRound(zeroColor.alpha() +
                           (referenceColor.alpha() - zeroColor.alpha()) * mask / 100.0);
                if (std::abs(actual.pixelColor(fill).alpha() - expectedAlpha) > 1) {
                    std::cerr << "control " << controls[index]->metaObject()->className() << ' '
                              << controls[index]->objectName().toStdString() << " mask " << mask
                              << " opacity "
                              << adqt::theme::ThemeManager::instance().backgroundOpacity(
                                     controls[index])
                              << " reference " << referenceColor.name(QColor::HexArgb).toStdString()
                              << " zero " << zeroColor.name(QColor::HexArgb).toStdString()
                              << " actual "
                              << actual.pixelColor(fill).name(QColor::HexArgb).toStdString()
                              << '\n';
                }
                require(std::abs(actual.pixelColor(fill).alpha() - expectedAlpha) <= 1,
                        "settings and search control backgrounds must track the skin mask");
                if (mask == 0) {
                    bool preservesInk = false;
                    for (int y = 0; y < actual.height(); ++y) {
                        for (int x = 0; x < actual.width(); ++x) {
                            preservesInk |= actual.pixelColor(x, y).alpha() > zeroColor.alpha();
                        }
                    }
                    require(preservesInk, "zero background opacity must preserve control ink");
                }
            }
            QWidget dialog(root, Qt::Dialog);
            adqt::widgets::AdButton dialogButton(&dialog);
            require(adqt::theme::ThemeManager::instance()
                            .resolveTheme(&dialogButton)
                            .backgroundOpacity == 1.0,
                    "owned dialogs without a skin must keep opaque control backgrounds");
        }
    }

    if (!previewDirectory.isEmpty()) {
        require(QDir().mkpath(previewDirectory), "create the requested main-window preview folder");
        const auto save = [&](const QString& name) {
            flushEvents();
            require(
                root->grab().save(QDir(previewDirectory).filePath(name + QStringLiteral(".png"))),
                "save the requested main-window skin preview");
        };
        for (const auto appearance :
             {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
            themeManager.setThemeAppearance(appearance);
            const QString theme = appearance == styles::ThemeAppearance::Light
                                      ? QStringLiteral("light")
                                      : QStringLiteral("dark");
            window.showSettingsLocation(QStringLiteral("interface-settings"),
                                        QStringLiteral("skin"));
            for (const QString& mode : {QStringLiteral("overlay"), QStringLiteral("contain")}) {
                require(
                    configuration.setValues({{QStringLiteral("interface/skin_display_mode"), mode},
                                             {QStringLiteral("interface/skin_blur_level"), 0},
                                             {QStringLiteral("interface/skin_mask_opacity"), 80}}),
                    "configure the main-window preview mode");
                waitUntil([&] {
                    return !controller->diagnostics().busy &&
                           window.findChild<presentation::MainWindowSkinWidget*>()->skinActive();
                });
                save(QStringLiteral("skin-%1-%2").arg(theme, mode));
                if (mode == QStringLiteral("overlay")) {
                    sidebar->setCollapsed(false);
                    save(QStringLiteral("skin-%1-sidebar-expanded").arg(theme));
                    sidebar->setCollapsed(true);
                    save(QStringLiteral("skin-%1-sidebar-collapsed").arg(theme));
                    sidebar->setCollapsed(false);
                }
            }
            require(configuration.setValues(
                        {{QStringLiteral("interface/skin_display_mode"), QStringLiteral("overlay")},
                         {QStringLiteral("interface/skin_blur_level"), 100}}),
                    "configure the maximum-blur preview");
            waitUntil([&] {
                return !controller->diagnostics().busy &&
                       window.findChild<presentation::MainWindowSkinWidget*>()->skinActive();
            });
            save(QStringLiteral("skin-%1-max-blur").arg(theme));
            window.showAbout();
            save(QStringLiteral("skin-%1-about").arg(theme));
            window.showScreenshotHistory();
            save(QStringLiteral("skin-%1-history").arg(theme));
            window.showPinToScreenManagement();
            save(QStringLiteral("skin-%1-pinned").arg(theme));

            require(configuration.setValues(
                        {{QStringLiteral("interface/skin_blur_level"), 0},
                         {QStringLiteral("interface/skin_mask_opacity"), 50},
                         {QStringLiteral("extended_features/translation_page_enabled"), true}}),
                    "configure clear skin previews for every main-interface page");
            waitUntil([&] {
                return !controller->diagnostics().busy && controller->maskOpacity() == 0.5;
            });
            for (const auto& page : registry.catalog().pages()) {
                card->navigateTo({page.id, {}, {}});
                require(card->currentLocation().pageId == page.id,
                        "visual review must visit every actual catalog page");
                save(QStringLiteral("page-%1-%2").arg(theme, page.id));
                for (const auto& section : page.sections) {
                    card->navigateTo({page.id, section.id, {}});
                    save(QStringLiteral("section-%1-%2-%3").arg(theme, page.id, section.id));
                }
            }
        }
    }

    themeManager.setThemeAppearance(previousAppearance);
    window.showFunctionSettings();
    require(configuration.setValues(savedSkin), "restore the original skin preferences");
    require(configuration.setValue(QStringLiteral("extended_features/translation_page_enabled"),
                                   savedTranslationPage),
            "restore the original translation-page preference");
    waitUntil(
        [&] { return presentation::MainWindowSkinController::existingInstance() == nullptr; });
    const QImage restored = root->grab().toImage();
    require(root->metaObject() == &QWidget::staticMetaObject && root->autoFillBackground() &&
                window.findChild<presentation::MainWindowSkinWidget*>() == nullptr,
            "removing the skin must remove its widget and restore the original Qt paint path");
    for (const auto& surface : samples) {
        require(sample(restored, *surface.surface, surface.point) ==
                    sample(original, *surface.surface, surface.point),
                "removing the skin must recover the original main-window surface colors");
    }
    require(sample(restored, *card, QPoint(-4, 40)) == sample(original, *card, QPoint(-4, 40)),
            "removing the skin must restore the original content-area background");
}

#ifdef Q_OS_MACOS
void standardCloseClosesMainWindow() {
    const auto& registry = settings::builtInSettingsRegistry();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    QPointer<MainWindow> window = new MainWindow(registry, session);
    window->show();
    require(triggerWindowCloseShortcut(window), "main window registers standard Close");
    flushEvents();
    require(!window, "standard Close disposes the main window");
}

void permissionRedirectShowsMainInterfacePrompt() {
    const auto& registry = settings::builtInSettingsRegistry();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    MainWindow window(registry, session);
    auto* messages = adqt::widgets::AdMessageService::instance(&window);
    QPointer<adqt::widgets::AdMessageHandle> prompt;
    QObject::connect(messages, &adqt::widgets::AdMessage::messageOpened, &window,
                     [&prompt](adqt::widgets::AdMessageHandle* handle) { prompt = handle; });

    window.showAppPermissions(QStringLiteral("accessibility"));
    flushEvents();
    auto* card = window.findChild<ContentCardWidget*>();
    require(window.isVisible() && card != nullptr &&
                card->currentLocation().pageId == QStringLiteral("app-permissions") &&
                card->currentLocation().itemId == QStringLiteral("accessibility"),
            "permission failure must open the main interface at the affected permission");
    require(prompt != nullptr && messages->count() == 1 &&
                prompt->key() == QStringLiteral("main-app-permission-required") &&
                prompt->type() == adqt::widgets::AdMessage::Type::Warning &&
                prompt->content() == QStringLiteral("Grant the required permission to continue"),
            "permission failure must show a warning through the main-interface message component");

    window.showAppPermissions(QStringLiteral("screen-recording"));
    flushEvents();
    require(messages->count() == 1 &&
                card->currentLocation().itemId == QStringLiteral("screen-recording"),
            "repeated permission failures must update one prompt while navigating to the latest "
            "permission");
}
#endif

void applicationTypographyCoversUnownedSurfaces() {
    // The theme owns application-wide typography: tooltips, message boxes, native menus,
    // and ownerless overlay-style windows all resolve unhinted outlines at fractional DPI.
    require(QApplication::font().hintingPreference() == QFont::PreferNoHinting,
            "the themed application font must render unhinted outlines");
    for (const char* popupClass : {"QTipLabel", "QMessageBox", "QMenu"}) {
        const QFont popupFont = QApplication::font(popupClass);
        require(popupFont.hintingPreference() == QFont::PreferNoHinting,
                "native popup class fonts must render unhinted outlines");
    }
    QWidget standalone; // models overlay, palette, pinned, and recognition windows
    require(standalone.font().hintingPreference() == QFont::PreferNoHinting,
            "parentless top-level widgets must inherit unhinted outlines");
    QMenu trayMenu; // seeds from the QMenu class font instead of an owner chain
    require(trayMenu.font().hintingPreference() == QFont::PreferNoHinting,
            "native menus must render unhinted outlines");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    const QDir fonts(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts"));
    QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("msyh.ttc")));
    QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("segoeui.ttf")));
#endif
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("main_window_font_tests"));
    QTemporaryDir directory;
    require(directory.isValid(), "isolated font test storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 8000}).success,
            "initialize isolated font test storage");
    styles::ThemeManager::instance().initialize(application);
    applicationTypographyCoversUnownedSurfaces();
    customTitleBarUsesPlatformWindowControls();
    titleBarBackgroundMatchesNavigationMenu();
    skinMasksCompositeOnceAndSurviveThemeChanges();
    mainWindowTitlesKeepSmoothRendering();
    QString skinPreviewDirectory;
    const QStringList arguments = application.arguments();
    const auto previewArgument = arguments.indexOf(QStringLiteral("--skin-previews"));
    if (previewArgument >= 0) {
        require(previewArgument + 1 < arguments.size(),
                "--skin-previews requires an output folder");
        skinPreviewDirectory = QDir(arguments.at(previewArgument + 1)).absolutePath();
    }
    mainWindowSkinIsContinuousAndRestoresTheme(skinPreviewDirectory);
#ifdef Q_OS_MACOS
    standardCloseClosesMainWindow();
    permissionRedirectShowsMainInterfacePrompt();
#endif
    storage.shutdown();
    return 0;
}
