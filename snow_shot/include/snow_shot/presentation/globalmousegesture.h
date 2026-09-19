#ifndef SNOW_SHOT_PRESENTATION_GLOBALMOUSEGESTURE_H
#define SNOW_SHOT_PRESENTATION_GLOBALMOUSEGESTURE_H

#include "snow_shot/presentation/globalmousetypes.h"

#include <QPointF>
#include <QVector>
#include <Qt>
#include <optional>

namespace snow_shot::presentation {
struct GlobalMouseBinding {
    settings::SettingsGlobalMouseAction action;
    GlobalMouseModifiers modifiers{};
    Qt::MouseButton button = Qt::NoButton;
};

[[nodiscard]] std::optional<GlobalMouseBinding>
globalMouseBinding(settings::SettingsGlobalMouseAction action,
                   const settings::SettingsGlobalMouseCombination& combination);

struct GlobalMouseConfiguration {
    QVector<GlobalMouseBinding> bindings;
    bool captureAvailable = false;
};

struct GlobalMouseInput {
    enum class Kind { Press, Move, Release, Cancel, Other };
    Kind kind = Kind::Other;
    QPointF position;
    Qt::MouseButton button = Qt::NoButton;
    GlobalMouseModifiers modifiers{};
    bool injected = false;
    Qt::MouseButtons heldButtons{};
};

struct GlobalMouseDragEvent {
    enum class Kind { Begin, Update, Finish, Cancel };
    Kind kind = Kind::Begin;
    quint64 id = 0;
    settings::SettingsGlobalMouseAction action =
        settings::SettingsGlobalMouseAction::ScreenshotCopy;
    QPointF position;
    GlobalMouseCoordinateSpace coordinateSpace = GlobalMouseCoordinateSpace::PhysicalPixels;
};

struct GlobalMouseInputResult {
    bool consumed = false;
    GlobalMouseModifiers activationModifiers;
    std::optional<GlobalMouseDragEvent> event;
};

class GlobalMouseGesture final {
  public:
    [[nodiscard]] GlobalMouseInputResult beginButtonDrag(settings::SettingsGlobalMouseAction action,
                                                         const QPointF& position);
    [[nodiscard]] GlobalMouseInputResult handle(const GlobalMouseInput& input,
                                                const GlobalMouseConfiguration& configuration);
    void cancel(quint64 id);
    [[nodiscard]] bool active() const {
        return m_activeId != 0;
    }
    [[nodiscard]] bool pending() const {
        return m_pendingId != 0;
    }
    [[nodiscard]] bool needsMouseInput() const {
        return active() || m_consumedButtons != Qt::NoButton;
    }
    [[nodiscard]] bool ownsButton(Qt::MouseButton button) const {
        return button != Qt::NoButton && m_consumedButtons.testFlag(button);
    }
    void reset() {
        const quint64 nextId = m_nextId;
        *this = {};
        m_nextId = nextId;
    }

  private:
    quint64 m_nextId = 0;
    quint64 m_activeId = 0;
    quint64 m_pendingId = 0;
    Qt::MouseButton m_button = Qt::NoButton;
    Qt::MouseButtons m_consumedButtons;
    settings::SettingsGlobalMouseAction m_action =
        settings::SettingsGlobalMouseAction::ScreenshotCopy;
};
} // namespace snow_shot::presentation
#endif
