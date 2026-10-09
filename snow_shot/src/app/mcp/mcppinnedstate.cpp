#include "mcppinnedstate_p.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/storage/pinnedwindowrepository.h"

#include <QSet>

namespace snow_shot::app::mcp {
QJsonObject pinnedWindowState(const storage::PinnedWindowRepository& repository,
                              const presentation::PinnedWindowGroupManager& groups,
                              const QString& id) {
    if (auto* window = groups.liveWindow(id)) {
        auto state = window->automationState();
        state.insert(QStringLiteral("hidden"), false);
        return state;
    }
    const auto summary = repository.summary(id);
    if (!summary)
        return {};
    return {{QStringLiteral("id"), id},
            {QStringLiteral("revision"), static_cast<qint64>(repository.revision())},
            {QStringLiteral("group_id"), summary->groupId},
            {QStringLiteral("visible"), false},
            {QStringLiteral("open"), false},
            {QStringLiteral("hidden"), summary->hidden},
            {QStringLiteral("updated_at"), summary->updatedUtc.toString(Qt::ISODateWithMs)},
            {QStringLiteral("closed"), summary->ignored}};
}

QJsonArray pinnedWindowList(const storage::PinnedWindowRepository& repository,
                            const presentation::PinnedWindowGroupManager& groups) {
    QJsonArray list;
    QSet<QString> found;
    for (const auto& summary : repository.summariesIncludingPending()) {
        found.insert(summary.id);
        const bool open = groups.hasWindow(summary.id);
        list.append(QJsonObject{
            {QStringLiteral("id"), summary.id},
            {QStringLiteral("group_id"), summary.groupId},
            {QStringLiteral("open"), open},
            {QStringLiteral("hidden"), !open && summary.hidden},
            {QStringLiteral("closed"), summary.ignored},
            {QStringLiteral("updated_at"), summary.updatedUtc.toString(Qt::ISODateWithMs)}});
    }
    for (auto* window : groups.liveWindows())
        if (!found.contains(window->persistenceId()) && groups.hasWindow(window->persistenceId()) &&
            window->sourcePinAvailable())
            list.append(QJsonObject{{QStringLiteral("id"), window->persistenceId()},
                                    {QStringLiteral("group_id"), window->groupId()},
                                    {QStringLiteral("open"), true},
                                    {QStringLiteral("hidden"), false}});
    return list;
}
} // namespace snow_shot::app::mcp
