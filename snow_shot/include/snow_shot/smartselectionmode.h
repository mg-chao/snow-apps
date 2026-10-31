#ifndef SNOW_SHOT_SMARTSELECTIONMODE_H
#define SNOW_SHOT_SMARTSELECTIONMODE_H

#include <QMetaType>
#include <QString>
#include <QStringView>

#include <optional>

namespace snow_shot {

enum class SmartSelectionMode {
    WindowSubElement,
    Window,
    Disabled,
};

[[nodiscard]] inline SmartSelectionMode smartSelectionModeFromId(QStringView id) {
    if (id == u"window")
        return SmartSelectionMode::Window;
    if (id == u"disabled")
        return SmartSelectionMode::Disabled;
    return SmartSelectionMode::WindowSubElement;
}

[[nodiscard]] inline QString smartSelectionModeId(SmartSelectionMode mode) {
    switch (mode) {
    case SmartSelectionMode::Window:
        return QStringLiteral("window");
    case SmartSelectionMode::Disabled:
        return QStringLiteral("disabled");
    case SmartSelectionMode::WindowSubElement:
        return QStringLiteral("window_sub_element");
    }
    return {};
}

// The MCP boolean predates the manual-only mode: false means window-only lookup.
[[nodiscard]] constexpr SmartSelectionMode
smartSelectionModeWithOverride(SmartSelectionMode configured, std::optional<bool> override) {
    if (!override)
        return configured;
    if (!*override)
        return SmartSelectionMode::Window;
    return configured == SmartSelectionMode::Disabled ? SmartSelectionMode::WindowSubElement
                                                      : configured;
}

} // namespace snow_shot

Q_DECLARE_METATYPE(snow_shot::SmartSelectionMode)

#endif // SNOW_SHOT_SMARTSELECTIONMODE_H
