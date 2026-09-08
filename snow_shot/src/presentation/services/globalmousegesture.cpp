#include "snow_shot/presentation/globalmousegesture.h"

#include <utility>

namespace snow_shot::presentation {
std::optional<GlobalMouseBinding>
globalMouseBinding(settings::SettingsGlobalMouseAction action,
                   const settings::SettingsGlobalMouseCombination& combination) {
    GlobalMouseBinding binding{action, {}, Qt::NoButton};
    if (combination.activationKeys.isEmpty()) {
        return std::nullopt;
    }
    for (const QString& key : combination.activationKeys) {
        if (key == QStringLiteral("windows")) {
            binding.modifiers |= Qt::MetaModifier;
        } else if (key == QStringLiteral("ctrl")) {
            binding.modifiers |= Qt::ControlModifier;
        } else if (key == QStringLiteral("alt")) {
            binding.modifiers |= Qt::AltModifier;
        } else if (key == QStringLiteral("shift")) {
            binding.modifiers |= Qt::ShiftModifier;
        } else {
            return std::nullopt;
        }
    }
    if (combination.mouseButton == QStringLiteral("left_drag")) {
        binding.button = Qt::LeftButton;
    } else if (combination.mouseButton == QStringLiteral("right_drag")) {
        binding.button = Qt::RightButton;
    } else if (combination.mouseButton == QStringLiteral("wheel_drag")) {
        binding.button = Qt::MiddleButton;
    } else if (combination.mouseButton == QStringLiteral("side_button_1_drag")) {
        binding.button = Qt::BackButton;
    } else if (combination.mouseButton == QStringLiteral("side_button_2_drag")) {
        binding.button = Qt::ForwardButton;
    } else {
        return std::nullopt;
    }
    return binding;
}

GlobalMouseInputResult
GlobalMouseGesture::beginButtonDrag(settings::SettingsGlobalMouseAction action,
                                    const QPoint& position) {
    if (pending() || m_consumedButtons != Qt::NoButton) {
        return {};
    }
    const GlobalMouseConfiguration direct{{{action, {}, Qt::LeftButton}}, true};
    auto result = handle({GlobalMouseInput::Kind::Press, position, Qt::LeftButton}, direct);
    if (result.event) {
        // Qt already received the button press, so its release must also reach Qt.
        m_consumedButtons &= ~Qt::MouseButtons(Qt::LeftButton);
        result.consumed = false;
    }
    return result;
}

GlobalMouseInputResult GlobalMouseGesture::handle(const GlobalMouseInput& input,
                                                  const GlobalMouseConfiguration& configuration) {
    GlobalMouseInputResult result;
    if (input.injected) {
        return result;
    }
    if (input.kind == GlobalMouseInput::Kind::Cancel) {
        if (pending()) {
            result.consumed = true;
            result.event =
                GlobalMouseDragEvent{GlobalMouseDragEvent::Kind::Cancel,
                                     std::exchange(m_pendingId, 0), m_action, input.position};
            m_activeId = 0;
        }
        return result;
    }
    // Drain every swallowed press even if capture was cancelled before its release.
    if (input.kind == GlobalMouseInput::Kind::Release && m_consumedButtons.testFlag(input.button)) {
        m_consumedButtons &= ~Qt::MouseButtons(input.button);
        result.consumed = true;
        if (active() && input.button == m_button) {
            result.event =
                GlobalMouseDragEvent{GlobalMouseDragEvent::Kind::Finish,
                                     std::exchange(m_activeId, 0), m_action, input.position};
        }
        return result;
    }
    if (active()) {
        result.consumed = true;
        if (input.kind == GlobalMouseInput::Kind::Press) {
            m_consumedButtons |= input.button;
        } else if (input.kind == GlobalMouseInput::Kind::Move) {
            // Windows must still advance the cursor. The swallowed button sequence
            // prevents this motion from becoming a drag in the foreground application.
            result.consumed = false;
            result.event = GlobalMouseDragEvent{GlobalMouseDragEvent::Kind::Update, m_activeId,
                                                m_action, input.position};
        } else if (input.kind == GlobalMouseInput::Kind::Release) {
            result.consumed = false;
            if (input.button == m_button) {
                result.event =
                    GlobalMouseDragEvent{GlobalMouseDragEvent::Kind::Finish,
                                         std::exchange(m_activeId, 0), m_action, input.position};
            }
        }
        return result;
    }
    if (input.kind != GlobalMouseInput::Kind::Press || pending() ||
        (input.heldButtons & ~Qt::MouseButtons(input.button)) != Qt::NoButton ||
        m_consumedButtons != Qt::NoButton || !configuration.captureAvailable) {
        return result;
    }
    const GlobalMouseBinding* match = nullptr;
    for (const auto& binding : configuration.bindings) {
        if (binding.button == input.button && binding.modifiers == input.modifiers) {
            if (match != nullptr) {
                return result;
            }
            match = &binding;
        }
    }
    if (match == nullptr) {
        return result;
    }
    m_activeId = ++m_nextId;
    m_pendingId = m_activeId;
    m_action = match->action;
    m_button = input.button;
    m_consumedButtons |= m_button;
    result.consumed = true;
    result.maskActivationKey =
        input.modifiers.testFlag(Qt::MetaModifier) || input.modifiers.testFlag(Qt::AltModifier);
    result.event = GlobalMouseDragEvent{GlobalMouseDragEvent::Kind::Begin, m_activeId, m_action,
                                        input.position};
    return result;
}

void GlobalMouseGesture::cancel(quint64 id) {
    if (m_pendingId == id) {
        m_pendingId = 0;
    }
    if (m_activeId == id) {
        m_activeId = 0;
    }
}
} // namespace snow_shot::presentation
