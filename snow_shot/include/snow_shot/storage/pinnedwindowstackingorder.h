#ifndef SNOW_SHOT_STORAGE_PINNEDWINDOWSTACKINGORDER_H
#define SNOW_SHOT_STORAGE_PINNEDWINDOWSTACKINGORDER_H

#include <QHash>
#include <QSet>
#include <QStringList>
#include <list>
#include <map>

namespace snow_shot::storage {

// Logical front-to-back order, independent of native handles, visibility and payload state.
// Iterators make the frequent single-window promotion/removal paths constant time.
class PinnedWindowStackingOrder final {
  public:
    PinnedWindowStackingOrder() = default;
    PinnedWindowStackingOrder(const PinnedWindowStackingOrder&) = delete;
    PinnedWindowStackingOrder& operator=(const PinnedWindowStackingOrder&) = delete;

    [[nodiscard]] quint64 revision() const {
        return m_revision;
    }
    [[nodiscard]] QStringList order(const QString& group) const {
        QStringList result;
        const auto found = m_groups.find(group);
        if (found != m_groups.end()) {
            result.reserve(static_cast<qsizetype>(found->second.size()));
            for (const auto& id : found->second)
                result.append(id);
        }
        return result;
    }
    [[nodiscard]] QHash<QString, QStringList> orders() const {
        QHash<QString, QStringList> result;
        for (const auto& [group, members] : m_groups)
            if (!members.empty())
                result.insert(group, order(group));
        return result;
    }
    void add(const QString& id, const QString& group) {
        auto found = m_members.find(id);
        if (found != m_members.end()) {
            if (found->group != group)
                move({id}, group);
            return;
        }
        auto& members = m_groups[group];
        members.push_front(id);
        m_members.insert(id, {group, members.begin()});
        ++m_revision;
    }
    void remove(const QString& id) {
        const auto found = m_members.find(id);
        if (found == m_members.end())
            return;
        m_groups.at(found->group).erase(found->position);
        m_members.erase(found);
        ++m_revision;
    }
    bool promote(const QString& id) {
        const auto found = m_members.constFind(id);
        if (found == m_members.cend())
            return false;
        auto& members = m_groups.at(found->group);
        if (found->position == members.begin())
            return false;
        members.splice(members.begin(), members, found->position);
        ++m_revision;
        return true;
    }
    // Preserve source order, not caller/container iteration order. Cross-group inputs use
    // deterministic source-group order. Members already in the destination remain in place.
    void move(const QSet<QString>& ids, const QString& destination) {
        std::list<QString> moved;
        for (auto& [group, members] : m_groups) {
            if (group == destination)
                continue;
            for (auto it = members.begin(); it != members.end();) {
                const auto current = it++;
                if (!ids.contains(*current))
                    continue;
                m_members[*current].group = destination;
                moved.splice(moved.end(), members, current);
            }
        }
        if (moved.empty())
            return;
        auto& target = m_groups[destination];
        target.splice(target.begin(), moved);
        ++m_revision;
    }
    // Reorder only observed members. Hidden, closed and not-yet-presented members keep
    // their slots. Stale observations cannot introduce or resurrect a member.
    bool observe(const QString& group, const QStringList& observed) {
        QStringList filtered;
        QSet<QString> selected;
        filtered.reserve(observed.size());
        for (const auto& id : observed) {
            const auto member = m_members.constFind(id);
            if (member != m_members.cend() && member->group == group && !selected.contains(id)) {
                selected.insert(id);
                filtered.append(id);
            }
        }
        if (filtered.size() < 2)
            return false;
        bool changed = false;
        qsizetype index = 0;
        auto& members = m_groups.at(group);
        for (auto it = members.begin(); it != members.end(); ++it) {
            if (!selected.contains(*it))
                continue;
            const QString& id = filtered.at(index++);
            changed |= *it != id;
            *it = id;
            m_members[id].position = it;
        }
        if (changed)
            ++m_revision;
        return changed;
    }

  private:
    struct Member {
        QString group;
        std::list<QString>::iterator position;
    };
    std::map<QString, std::list<QString>> m_groups;
    QHash<QString, Member> m_members;
    quint64 m_revision = 0;
};

} // namespace snow_shot::storage
#endif
