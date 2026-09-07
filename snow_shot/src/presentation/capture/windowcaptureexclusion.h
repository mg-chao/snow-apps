#ifndef SNOW_SHOT_PRESENTATION_WINDOWCAPTUREEXCLUSION_H
#define SNOW_SHOT_PRESENTATION_WINDOWCAPTUREEXCLUSION_H

#include <QPointer>
#include <QWidget>

#include <functional>
#include <utility>
#include <vector>

namespace snow_shot::presentation {

class WindowCaptureExclusion final {
  public:
    using Setter = std::function<bool(QWidget*, bool)>;

    explicit WindowCaptureExclusion(Setter setter = {}) : m_setter(std::move(setter)) {}

    // Exclusion is best effort and must never change a window's visibility.
    void exclude(QWidget* window) {
        if (window != nullptr && m_setter && m_setter(window, true)) {
            m_windows.emplace_back(window);
        }
    }

    void restore() {
        for (auto window = m_windows.rbegin(); window != m_windows.rend(); ++window) {
            if (!window->isNull()) {
                static_cast<void>(m_setter(window->data(), false));
            }
        }
        m_windows.clear();
    }

  private:
    Setter m_setter;
    std::vector<QPointer<QWidget>> m_windows;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_WINDOWCAPTUREEXCLUSION_H
