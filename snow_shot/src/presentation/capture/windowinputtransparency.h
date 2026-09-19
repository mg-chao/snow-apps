#ifndef SNOW_SHOT_PRESENTATION_WINDOWINPUTTRANSPARENCY_H
#define SNOW_SHOT_PRESENTATION_WINDOWINPUTTRANSPARENCY_H

#include <QPointer>
#include <QWidget>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace snow_shot::presentation {

// The setter returns the previous input state, or nullopt without changing it on failure.
class WindowInputTransparency final {
  public:
    using Setter = std::function<std::optional<bool>(QWidget*, bool)>;

    explicit WindowInputTransparency(Setter setter) : m_setter(std::move(setter)) {}
    ~WindowInputTransparency() {
        restore();
    }
    WindowInputTransparency(const WindowInputTransparency&) = delete;
    WindowInputTransparency& operator=(const WindowInputTransparency&) = delete;

    bool enable(QWidget* window) {
        if (window == nullptr || !m_setter) {
            return false;
        }
        for (const auto& entry : m_windows) {
            if (entry.window == window && entry.id == window->internalWinId()) {
                return true;
            }
        }
        const auto previous = m_setter(window, true);
        if (!previous.has_value()) {
            return false;
        }
        m_windows.push_back({window, window->internalWinId(), previous.value()});
        return true;
    }

    void restore() {
        for (auto entry = m_windows.rbegin(); entry != m_windows.rend(); ++entry) {
            // A replacement native window must not inherit the old window's input state.
            if (entry->window && entry->window->internalWinId() == entry->id) {
                if (!m_setter(entry->window.data(), entry->previous).has_value()) {
                    qWarning("Could not restore window input transparency");
                }
            }
        }
        m_windows.clear();
    }

  private:
    struct Entry {
        QPointer<QWidget> window;
        WId id;
        bool previous;
    };
    Setter m_setter;
    std::vector<Entry> m_windows;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_WINDOWINPUTTRANSPARENCY_H
