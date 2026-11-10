#ifndef SNOW_SHOT_APP_MCP_MCPSTARTUPERROR_H
#define SNOW_SHOT_APP_MCP_MCPSTARTUPERROR_H

#include <QCoreApplication>
#include <QMetaType>
#include <QString>

namespace snow_shot::app::mcp {

// Retain the failure independently of the current language, including after the failed
// service graph has been retired. Translate only when presenting it to a user.
enum class McpStartupError { None, RuntimeDirectory, EndpointOwned, LocalEndpoint, Descriptor };

[[nodiscard]] inline QString mcpStartupErrorText(McpStartupError error) {
    switch (error) {
    case McpStartupError::None:
        return {};
    case McpStartupError::RuntimeDirectory:
        return QCoreApplication::translate("snow_shot::app::mcp::ScreenshotMcpServer",
                                           "Could not secure the MCP runtime directory");
    case McpStartupError::EndpointOwned:
        return QCoreApplication::translate("snow_shot::app::mcp::ScreenshotMcpServer",
                                           "Another Snow Shot instance owns the MCP endpoint");
    case McpStartupError::LocalEndpoint:
        return QCoreApplication::translate("snow_shot::app::mcp::ScreenshotMcpServer",
                                           "Could not open the local MCP endpoint");
    case McpStartupError::Descriptor:
        return QCoreApplication::translate("snow_shot::app::mcp::ScreenshotMcpServer",
                                           "Could not write the private MCP descriptor");
    }
    return {};
}

} // namespace snow_shot::app::mcp

Q_DECLARE_METATYPE(snow_shot::app::mcp::McpStartupError)

#endif // SNOW_SHOT_APP_MCP_MCPSTARTUPERROR_H
