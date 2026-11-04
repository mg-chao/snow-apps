#include "snow_shot/app/distribution.h"
#include "snow_shot/presentation/components/settingscustomwidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include "widgets/button.h"
#include "widgets/input_text_edit.h"
#include "widgets/tag.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QScrollBar>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace settings = snow_shot::presentation::settings;
namespace styles = snow_shot::presentation::styles;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class McpTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (qstrcmp(context, "ScreenshotMcpSettings") == 0) {
            return QStringLiteral("Translated: %1").arg(QString::fromUtf8(source));
        }
        return {};
    }
};

void mcpSettingsWidget() {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto& registry = settings::builtInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    const auto renderer = settings::SettingsCustomRenderer::McpStatus;
    const auto* field = registry.fieldForCustom(renderer);
    require(field != nullptr, "MCP field exists in the built-in registry");
    qApp->setProperty("snowShotMcpRunning", false);
    qApp->setProperty("snowShotMcpConnections", 0);
    std::unique_ptr<SettingsCustomWidget> widget(
        createSettingsCustomWidget(renderer, registry, *field->definition, session));
    auto* status = widget->findChild<adqt::widgets::AdTag*>(QStringLiteral("settings-mcp-status"));
    auto* clients = widget->findChild<QLabel*>(QStringLiteral("settings-mcp-clients"));
    auto* config =
        widget->findChild<adqt::widgets::AdTextEdit*>(QStringLiteral("settings-mcp-config"));
    auto* copy = widget->findChild<adqt::widgets::AdButton*>(QStringLiteral("settings-mcp-copy"));
    auto* endpoint = widget->findChild<QLabel*>(QStringLiteral("settings-mcp-endpoint"));
    require(status && clients && config && copy && endpoint,
            "MCP exposes Ant Design status, input, button and endpoint descriptor");
    require(status->text() == QStringLiteral("Unavailable"), "stopped service is unavailable");
    require(config->isReadOnly(), "client configuration is read-only");
    require(config->lineWrapMode() == QTextEdit::WidgetWidth,
            "client configuration wraps at the widget width");
    const QString json = config->toPlainText();
    const auto server = QJsonDocument::fromJson(json.toUtf8())
                            .object()
                            .value(QStringLiteral("mcpServers"))
                            .toObject()
                            .value(QStringLiteral("snow-shot"))
                            .toObject();
    require(QDir::isAbsolutePath(server.value(QStringLiteral("command")).toString()) &&
                server.value(QStringLiteral("args")).isArray(),
            "configuration contains an absolute bridge command and arguments array");
    QString expectedStoreDescriptor;
    if (snow_shot::app::distribution::isStoreManaged()) {
        const QString localAppData =
            QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"));
        require(QDir::isAbsolutePath(localAppData), "Store fixture has an absolute LOCALAPPDATA");
        const QString expectedCommand =
            QDir(localAppData)
                .filePath(QStringLiteral("Microsoft/WindowsApps/snow-shot-store-mcp.exe"));
        expectedStoreDescriptor = QDir::toNativeSeparators(
            QDir(localAppData)
                .filePath(QStringLiteral(
                    "Packages/SnowApps.SnowShot_3pp9w6ncw7bs0/LocalState/mcp/snow-shot-mcp.json")));
        require(server.value(QStringLiteral("command")).toString() == expectedCommand &&
                    server.value(QStringLiteral("args")).toArray().isEmpty(),
                "Store JSON uses the stable execution alias without installation-path arguments");
        require(endpoint->text() ==
                    QStringLiteral("Local endpoint descriptor: %1").arg(expectedStoreDescriptor),
                "Store endpoint uses the package LocalState path rather than the installation");
    }
    copy->click();
    require(QApplication::clipboard()->text() == json && copy->text() == QStringLiteral("Copied"),
            "copy publishes the exact configuration and confirms success");

    qApp->setProperty("snowShotMcpRunning", true);
    qApp->setProperty("snowShotMcpConnections", 2);
    widget->resize(680, widget->sizeHint().height());
    widget->show();
    QApplication::processEvents();
    require(status->text() == QStringLiteral("Running") &&
                status->colorScheme() == adqt::widgets::AdTag::ColorScheme::Success &&
                clients->text() == QStringLiteral("Connected clients: 2"),
            "opening the page refreshes the live connection status immediately");
    qApp->setProperty("snowShotMcpConnections", 3);
    const auto timers = widget->findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
    require(timers.size() == 1, "MCP has one status refresh timer");
    QMetaObject::invokeMethod(timers.front(), "timeout", Qt::DirectConnection);
    require(clients->text() == QStringLiteral("Connected clients: 3"),
            "visible status refresh reflects changed client count");

    McpTranslator translator;
    require(QCoreApplication::installTranslator(&translator), "MCP test translator installs");
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(widget.get(), &languageChange);
    require(copy->text() == QStringLiteral("Translated: Copy configuration"),
            "language changes update copy label");
    require(status->text() == QStringLiteral("Translated: Running"),
            "language changes update status label");
    require(config->isReadOnly(), "language changes preserve the textarea read-only mode");
    require(config->toPlainText() == json, "language changes preserve configuration JSON");
    if (!expectedStoreDescriptor.isEmpty()) {
        require(endpoint->text() == QStringLiteral("Translated: Local endpoint descriptor: %1")
                                        .arg(expectedStoreDescriptor),
                "language changes preserve the stable Store endpoint descriptor path");
    }
    QCoreApplication::removeTranslator(&translator);
    QApplication::sendEvent(widget.get(), &languageChange);

    const QString previewDirectory = qEnvironmentVariable("SNOW_MCP_SETTINGS_PREVIEW_DIR");
    for (const auto appearance : {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
        styles::ThemeStyleConfig theme;
        theme.appearance = appearance;
        styles::ThemeManager::instance().setThemeStyleConfig(theme);
        const auto scheme = styles::generateThemeColorScheme(theme);
        widget->applyTheme(scheme);
        auto palette = widget->palette();
        palette.setColor(QPalette::Window, scheme.map.colorBgContainer);
        widget->setPalette(palette);
        widget->setAutoFillBackground(true);
        for (const int width : {680, 400}) {
            widget->resize(width, qMax(widget->sizeHint().height(),
                                       widget->layout()->totalHeightForWidth(width)));
            QApplication::processEvents();
            require(config->width() <= widget->width() &&
                        copy->geometry().right() < widget->width(),
                    "configuration and copy action fit narrow layouts");
            require(config->horizontalScrollBar()->maximum() == 0,
                    "wrapped configuration never overflows horizontally");
            if (!previewDirectory.isEmpty()) {
                const QString name =
                    QStringLiteral("mcp-%1-%2.png")
                        .arg(appearance == styles::ThemeAppearance::Light ? QStringLiteral("light")
                                                                          : QStringLiteral("dark"))
                        .arg(width);
                require(widget->grab().save(QDir(previewDirectory).filePath(name)),
                        "save requested MCP preview");
            }
        }
    }
    qApp->setProperty("snowShotMcpRunning", false);
    QMetaObject::invokeMethod(timers.front(), "timeout", Qt::DirectConnection);
    require(status->colorScheme() == adqt::widgets::AdTag::ColorScheme::Default &&
                clients->text() == QStringLiteral("MCP is disabled or unavailable"),
            "stopping the service clears the connected state");
}
} // namespace

int main(int argc, char** argv) {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory exists");
    const bool hadLocalAppData = qEnvironmentVariableIsSet("LOCALAPPDATA");
    const QByteArray previousLocalAppData = qgetenv("LOCALAPPDATA");
    const auto restoreEnvironment = qScopeGuard([hadLocalAppData, previousLocalAppData] {
        if (hadLocalAppData)
            qputenv("LOCALAPPDATA", previousLocalAppData);
        else
            qunsetenv("LOCALAPPDATA");
    });
    // ThemeManager construction reads InterfaceSettings, which lazily initializes storage.
    // Establish the hermetic environment before Qt/product startup, and initialize storage
    // exactly once before any theme/settings consumers can observe its repositories.
    require(qputenv("LOCALAPPDATA", QDir::toNativeSeparators(directory.path()).toUtf8()),
            "the fixture isolates LOCALAPPDATA before product initialization");
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("mcp_settings_widget_tests"));
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(!storage.isInitialized(), "the fixture owns the first storage initialization");
    require(storage.initialize({directory.path(), directory.path(), 8000}).success,
            "initialize isolated MCP settings storage before theme consumers");
    styles::ThemeManager::instance().initialize(application);
    require(storage.configurationDirectory() == directory.path(),
            "theme initialization preserves the fixture storage lifetime");
    mcpSettingsWidget();
    storage.shutdown();
    return 0;
}
