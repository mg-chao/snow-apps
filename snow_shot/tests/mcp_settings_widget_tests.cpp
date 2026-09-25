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
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QScrollBar>
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
    require(status && clients && config && copy, "MCP exposes Ant Design status, input and button");
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
                clients->text() == QStringLiteral("MCP is disabled or unavailable."),
            "stopping the service clears the connected state");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("mcp_settings_widget_tests"));
    styles::ThemeManager::instance().initialize(application);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory exists");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    static_cast<void>(storage.initialize({directory.path(), directory.path(), 8000}));
    mcpSettingsWidget();
    storage.shutdown();
    return 0;
}
