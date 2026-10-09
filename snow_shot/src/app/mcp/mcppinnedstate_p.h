#ifndef SNOW_SHOT_APP_MCP_MCPPINNEDSTATE_P_H
#define SNOW_SHOT_APP_MCP_MCPPINNEDSTATE_P_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace snow_shot::storage {
class PinnedWindowRepository;
}
namespace snow_shot::presentation {
class PinnedWindowGroupManager;
}

namespace snow_shot::app::mcp {
QJsonObject pinnedWindowState(const storage::PinnedWindowRepository& repository,
                              const presentation::PinnedWindowGroupManager& groups,
                              const QString& id);
QJsonArray pinnedWindowList(const storage::PinnedWindowRepository& repository,
                            const presentation::PinnedWindowGroupManager& groups);
} // namespace snow_shot::app::mcp

#endif
