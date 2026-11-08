#ifndef SNOW_SHOT_APP_QUIESCENCESESSION_H
#define SNOW_SHOT_APP_QUIESCENCESESSION_H

#include <QDebug>
#include <functional>
#include <utility>
#include <vector>

namespace snow_shot::app {
// Own only successfully acquired pause handles. Release in reverse order once, even after
// partial preparation. Transaction state and commitment remain in storage/the updater.
class QuiescenceSession final {
  public:
    QuiescenceSession() = default;
    QuiescenceSession(const QuiescenceSession&) = delete;
    QuiescenceSession& operator=(const QuiescenceSession&) = delete;
    ~QuiescenceSession() {
        restore();
    }
    template <typename Restore> void acquired(Restore restore) {
        try {
            m_restore.emplace_back(restore);
        } catch (...) {
            restore();
            throw;
        }
    }
    void restore() noexcept {
        auto callbacks = std::move(m_restore);
        m_restore.clear();
        for (auto it = callbacks.rbegin(); it != callbacks.rend(); ++it) {
            try {
                (*it)();
            } catch (...) {
                // Continue releasing independent participants. Production restore hooks are
                // required not to throw; a violating hook must not strand the other handles.
                qWarning("Application pause restoration failed");
            }
        }
    }
    // A committed terminal handoff must not reopen admission during destruction.
    void commit() {
        m_restore.clear();
    }

  private:
    std::vector<std::function<void()>> m_restore;
};
} // namespace snow_shot::app
#endif
