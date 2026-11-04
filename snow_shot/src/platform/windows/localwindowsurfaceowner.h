// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PLATFORM_WINDOWS_LOCALWINDOWSURFACEOWNER_H
#define SNOW_SHOT_PLATFORM_WINDOWS_LOCALWINDOWSURFACEOWNER_H

#include <QHash>
#include <QPointer>
#include <QWidget>
#include <optional>

namespace snow_shot::presentation {

// GUI-thread-only ownership for native surfaces which are not QWidgets. A binding never
// owns the widget; its token prevents delayed teardown from removing a replacement binding.
class LocalWindowSurfaceOwnerBinding final {
  public:
    LocalWindowSurfaceOwnerBinding() = default;
    ~LocalWindowSurfaceOwnerBinding() {
        reset();
    }
    LocalWindowSurfaceOwnerBinding(const LocalWindowSurfaceOwnerBinding&) = delete;
    LocalWindowSurfaceOwnerBinding& operator=(const LocalWindowSurfaceOwnerBinding&) = delete;

    void bind(WId id, QWidget* owner) {
        const auto existing = bindings().constFind(m_id);
        if (id == m_id && existing != bindings().cend() && existing->token == m_token &&
            existing->owner == owner)
            return;
        reset();
        if (!id || !owner)
            return;
        m_id = id;
        m_token = ++nextToken();
        bindings().insert(id, {owner, m_token});
    }

    void reset() {
        const auto current = bindings().find(m_id);
        if (current != bindings().end() && current->token == m_token)
            bindings().erase(current);
        m_id = 0;
        m_token = 0;
    }

    // An empty optional denotes an unrelated surface. A registered surface with an expired
    // owner remains recognized until retirement, so commands must not use a native fallback.
    [[nodiscard]] static std::optional<QPointer<QWidget>> ownerForSurface(WId id) {
        const auto current = bindings().constFind(id);
        if (current == bindings().cend())
            return std::nullopt;
        return current->owner;
    }

  private:
    struct Entry {
        QPointer<QWidget> owner;
        quint64 token;
    };
    static QHash<WId, Entry>& bindings() {
        static QHash<WId, Entry> values;
        return values;
    }
    static quint64& nextToken() {
        static quint64 value = 0;
        return value;
    }
    WId m_id = 0;
    quint64 m_token = 0;
};

} // namespace snow_shot::presentation
#endif
