#ifndef SNOW_SHOT_STORAGE_PINNEDSOURCEIDENTITY_H
#define SNOW_SHOT_STORAGE_PINNEDSOURCEIDENTITY_H

#include <QString>

namespace snow_shot::storage {

// Empty keys identify legacy records and pins outside clipboard/file pinning.
// File keys contain the normalized original path, never the repository's copy.
// Clipboard keys contain a process-session token and a clipboard generation.
struct PinnedSourceIdentity final {
    QString key;

    [[nodiscard]] bool isValid() const {
        return !key.isEmpty();
    }
    friend bool operator==(const PinnedSourceIdentity&, const PinnedSourceIdentity&) = default;
};

} // namespace snow_shot::storage

#endif
