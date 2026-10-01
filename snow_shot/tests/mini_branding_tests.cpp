#include "snow_shot/app/edition.h"
#include "snow_shot/app/updateconfirmationdialog.h"
#include "snow_shot/presentation/components/aboutpagewidget.h"
#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/settingscustomwidget.h"
#include "snow_shot/presentation/components/formfields.h"
#include "snow_shot/presentation/components/titlebarwidget.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include "widgets/input_text_edit.h"
#include "icon_renderer.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QImage>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <memory>

namespace {
namespace edition = snow_shot::app::edition;
namespace settings = snow_shot::presentation::settings;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void flushEvents() {
    for (int pass = 0; pass < 4; ++pass) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

template <typename T> T* child(QObject& owner, const char* name) {
    auto* result = owner.findChild<T*>(QString::fromLatin1(name));
    require(result != nullptr, name);
    return result;
}

void titleBarRendersTheEditionSvg(TitleBarWidget& titleBar) {
    namespace styles = snow_shot::presentation::styles;
    namespace icons = snow_shot::presentation::icons::custom;
    auto& manager = styles::ThemeManager::instance();
    const auto originalAppearance = manager.themeColorScheme().appearance;
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        manager.setThemeAppearance(appearance);
        flushEvents();
        const auto scheme = manager.themeColorScheme();
        const QImage actual = titleBar.grab().toImage();
        const qreal scale = actual.devicePixelRatio();
        QColor ink = scheme.map.colorText;
#ifdef Q_OS_WIN
        if (!titleBar.window()->isActiveWindow()) {
            ink = scheme.map.colorTextTertiary;
        }
#endif
        const auto colors = adqt::icons::IconColors::primary(ink);
        const auto logo = edition::isMini ? icons::brand::SnowShotMiniLogo(colors)
                                          : icons::brand::SnowShotLogo(colors);
        const int height = std::clamp(scheme.metricAlias.fontSizeSM, 10, 14);
        adqt::icons::IconRenderRequest request;
        request.logicalSize =
            QSize(qRound(height * (edition::isMini ? 137.0 : 95.0) / 17.0), height);
        request.devicePixelRatio = scale;
        const QPixmap pixmap = adqt::icons::renderIconPixmap(logo, request);
        QImage expected(actual.size(), QImage::Format_ARGB32_Premultiplied);
        expected.setDevicePixelRatio(scale);
        expected.fill(titleBar.palette().color(QPalette::Window));
#ifdef Q_OS_WIN
        const QPointF position(48,
                               qRound((titleBar.height() * scale - pixmap.height()) / 2.0) / scale);
#else
        const QPointF position((titleBar.width() - qRound(pixmap.width() / scale)) / 2.0,
                               (titleBar.height() - qRound(pixmap.height() / scale)) / 2.0);
#endif
        {
            QPainter painter(&expected);
#ifndef Q_OS_WIN
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
#endif
            painter.drawPixmap(position, pixmap);
        }
        const QRect bounds = QRectF(position * scale, QSizeF(pixmap.size())).toAlignedRect();
        require(
            actual.copy(bounds).convertToFormat(expected.format()) == expected.copy(bounds),
            "both title bars must render their vector wordmark exactly in light and dark themes");
    }
    manager.setThemeAppearance(originalAppearance);
    flushEvents();
}

void brandingRetranslatesWithoutChangingIdentifiers() {
    const auto& registry = settings::builtInSettingsRegistry();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(registry, backend);
    const auto renderer = settings::SettingsCustomRenderer::McpStatus;
    const auto* mcpField = registry.fieldForCustom(renderer);
    require(mcpField != nullptr, "both editions retain MCP client configuration");
    std::unique_ptr<SettingsCustomWidget> mcp(
        createSettingsCustomWidget(renderer, registry, *mcpField->definition, session));
    auto* mcpConfig = child<adqt::widgets::AdTextEdit>(*mcp, "settings-mcp-config");
    namespace fields = snow_shot::presentation::components::form_fields;
    auto* configurationField = mcp->findChild<fields::FormField*>();
    require(configurationField != nullptr && configurationField->controlWidget() == mcpConfig &&
                configurationField->metadata().id == QStringLiteral("settings-mcp-config") &&
                mcpConfig->isReadOnly(),
            "both editions use a shared read-only field for MCP configuration");
    if (edition::isMini) {
        for (const auto unavailable :
             {settings::SettingsCustomRenderer::CustomAiModels,
              settings::SettingsCustomRenderer::TextTranslationConfigurations}) {
            std::unique_ptr<SettingsCustomWidget> widget(
                createSettingsCustomWidget(unavailable, registry, *mcpField->definition, session));
            require(widget == nullptr, "Mini does not construct Full-only API settings widgets");
        }
    }
    auto* mcpHelp = child<QLabel>(*mcp, "settings-mcp-help");
    auto* mcpEndpoint = child<QLabel>(*mcp, "settings-mcp-endpoint");
    const QString mcpJson = mcpConfig->toPlainText();
    const auto mcpServers = QJsonDocument::fromJson(mcpJson.toUtf8())
                                .object()
                                .value(QStringLiteral("mcpServers"))
                                .toObject();
    require(mcpServers.size() == 1 && mcpServers.contains(edition::productId()),
            "MCP client configuration identifies only the compiled edition");
    const auto mcpServer = mcpServers.value(edition::productId()).toObject();
    require(mcpServer.value(QStringLiteral("command")).toString() ==
                    QDir(QCoreApplication::applicationDirPath()).filePath(edition::mcpName()) &&
                mcpServer.value(QStringLiteral("args")).isArray(),
            "MCP client configuration launches the edition's adjacent bridge");
    const QString descriptor =
        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
            .filePath(edition::registryName() + QStringLiteral("/mcp/") + edition::productId() +
                      QStringLiteral("-mcp.json"));
    MainWindow window(registry, session);
    window.show();
    auto* card = window.findChild<ContentCardWidget*>();
    require(card != nullptr, "main window contains its navigation content");
    card->setCurrentRoute(QStringLiteral("/about"));
    flushEvents();
    auto* embedded = window.findChild<AboutPageWidget*>();
    auto* titleBar = window.findChild<TitleBarWidget*>();
    require(embedded != nullptr && titleBar != nullptr,
            "main window constructs About and title bar");
    titleBarRendersTheEditionSvg(*titleBar);
    AboutPageWidget standalone;

    struct Case {
        const char* locale;
        const char* about;
        const char* logo;
    };
    const Case cases[] = {
        {"en_US", "About %1", "%1 logo"},
        {"zh_CN", "关于 %1", "%1 标志"},
        {"zh_TW", "關於 %1", "%1 標誌"},
    };
    const QString product =
        edition::isMini ? QStringLiteral("Snow Shot Mini") : QStringLiteral("Snow Shot");
    const QString windowName = edition::isMini ? product : QStringLiteral("SnowShot");
    for (const auto& test : cases) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(SNOW_SHOT_TEST_TRANSLATIONS_DIR) +
                    QStringLiteral("/snow_shot_%1.qm").arg(QString::fromLatin1(test.locale))),
                "load complete branding catalog");
        require(QCoreApplication::installTranslator(&translator), "install branding catalog");
        flushEvents();

        require(edition::productName() == product,
                "product branding must remain exact in every locale");
        require(mcpConfig->toPlainText() == mcpJson,
                "MCP bridge identity remains stable across language changes");
        const char* mcpHelpSource =
            edition::isMini
                ? "Add this configuration to your MCP client, then restart the client to connect. "
                  "Keep %1 running while using MCP."
                : "Add this configuration to your MCP client, then restart the client to connect. "
                  "Keep Snow Shot running while using MCP.";
        const QString translatedMcpHelp =
            translator.translate("ScreenshotMcpSettings", mcpHelpSource);
        require(!translatedMcpHelp.isEmpty(), "all catalogs include edition-specific MCP help");
        require(mcpHelp->text() ==
                        (edition::isMini ? translatedMcpHelp.arg(product) : translatedMcpHelp) &&
                    mcpHelp->text().contains(product),
                "MCP help names the edition to keep running in every language");
        const QString translatedEndpoint =
            translator.translate("ScreenshotMcpSettings", "Local endpoint descriptor: %1");
        require(!translatedEndpoint.isEmpty() &&
                    mcpEndpoint->text() ==
                        translatedEndpoint.arg(QDir::toNativeSeparators(descriptor)),
                "MCP settings advertises the edition's isolated endpoint descriptor");
        require(window.windowTitle() == windowName && window.accessibleName() == windowName,
                "Mini main window uses its product; Full retains historical SnowShot identifiers");
        require(window.objectName() == QStringLiteral("snowShotMainWindow"),
                "main window object identifier stays compatible in both editions");
        if (edition::isMini) {
            require(titleBar->accessibleName() == product,
                    "Mini title bar exposes its own product name");
        }
        const QString expectedAbout = QString::fromUtf8(test.about).arg(product);
        const QString expectedLogo = QString::fromUtf8(test.logo).arg(product);
        require(
            translator.translate("AboutPageWidget", "About %1") == QString::fromUtf8(test.about) &&
                translator.translate("AboutPageWidget", "%1 logo") == QString::fromUtf8(test.logo),
            "all catalogs include the reusable Mini branding strings");
        for (AboutPageWidget* page : {embedded, &standalone}) {
            require(page->accessibleName() == expectedAbout,
                    "existing standalone and embedded About pages retranslate their product name");
            require(child<QLabel>(*page, "aboutProductName")->text() == product,
                    "About displays the exact edition name");
            auto* logo = child<QLabel>(*page, "aboutLogo");
            require(
                logo->accessibleName() == expectedLogo && !logo->pixmap().isNull(),
                "About keeps visible logo artwork and retranslates its accessible product name");
            require(page->objectName() == QStringLiteral("aboutPage"),
                    "About page keeps its stable object identifier");
        }
        require(card->currentRoute() == QStringLiteral("/about"),
                "language changes preserve the stable About route");

        bool inspected = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            require(dialog != nullptr, "update confirmation opens its testable Qt dialog");
            const char* source =
                edition::isMini
                    ? "%1 will close and restart to install the update. Continue?"
                    : "Snow Shot will close and restart to install the update. Continue?";
            const QString translated =
                translator.translate("snow_shot::app::ApplicationController", source);
            require(!translated.isEmpty(), "update confirmation branding exists in every catalog");
            const QString expected = edition::isMini ? translated.arg(product) : translated;
            require(dialog->text() == expected && dialog->text().contains(product),
                    "update confirmation names the selected product in every language");
            require(dialog->defaultButton() == dialog->button(QMessageBox::Cancel),
                    "update confirmation preserves its cancel default");
            inspected = true;
            dialog->button(QMessageBox::Cancel)->click();
        });
        require(!snow_shot::app::confirmRestartAndUpdate(&window) && inspected,
                "branding inspection cancels without applying an update");

        QCoreApplication::removeTranslator(&translator);
        flushEvents();
        require(embedded->accessibleName() == QStringLiteral("About %1").arg(product) &&
                    standalone.accessibleName() == QStringLiteral("About %1").arg(product),
                "removing a catalog restores existing About pages to the English source");
    }
    window.hide();
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    const QDir fonts(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts"));
    for (const QString& file : {QStringLiteral("segoeui.ttf"), QStringLiteral("seguisb.ttf"),
                                QStringLiteral("msyh.ttc"), QStringLiteral("msyhbd.ttc")}) {
        if (QFileInfo::exists(fonts.filePath(file))) {
            QFontDatabase::addApplicationFont(fonts.filePath(file));
        }
    }
#endif
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("mini_branding_tests"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.2.3"));
    QTemporaryDir directory;
    require(directory.isValid(), "create isolated branding test storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 8000}).success,
            "initialize isolated branding test storage");
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);
    brandingRetranslatesWithoutChangingIdentifiers();
    storage.shutdown();
    return 0;
}
