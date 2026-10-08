#ifndef SNOW_SHOT_PRESENTATION_PINNEDDISPLAYSELECTION_H
#define SNOW_SHOT_PRESENTATION_PINNEDDISPLAYSELECTION_H

#include "snow_shot/storage/pinnedwindowplacement.h"
#include <optional>
#include <span>

namespace snow_shot::presentation {
struct PinnedDisplayIdentity {
    QString name;
    QString serial;
};

[[nodiscard]] inline std::optional<std::size_t>
pinnedDisplayIndex(const storage::PinnedWindowPlacement& placement,
                   std::span<const PinnedDisplayIdentity> displays) {
    std::optional<std::size_t> exactMatch;
    std::optional<std::size_t> serialMatch;
    std::optional<std::size_t> nameMatch;
    std::size_t exactCount = 0;
    std::size_t serialCount = 0;
    std::size_t nameCount = 0;
    for (std::size_t index = 0; index < displays.size(); ++index) {
        const bool sameName =
            !placement.displayName.isEmpty() && displays[index].name == placement.displayName;
        const bool sameSerial =
            !placement.displaySerial.isEmpty() && displays[index].serial == placement.displaySerial;
        if (sameName && sameSerial) {
            exactMatch = index;
            ++exactCount;
        }
        if (sameSerial) {
            serialMatch = index;
            ++serialCount;
        }
        if (sameName) {
            nameMatch = index;
            ++nameCount;
        }
    }
    // Serial numbers can be duplicated. Prefer the complete identity, while
    // allowing a unique serial to follow a renamed display. Ambiguous or missing
    // identities belong to the caller's fallback policy, never enumeration order.
    if (exactCount == 1)
        return exactMatch;
    if (serialCount == 1)
        return serialMatch;
    if (nameCount == 1)
        return nameMatch;
    return std::nullopt;
}
} // namespace snow_shot::presentation
#endif
