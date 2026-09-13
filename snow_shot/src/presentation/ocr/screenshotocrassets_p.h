#pragma once

#include <functional>

class QLockFile;

namespace snow_shot::presentation::detail {
// Preserve the caller's stale-lock policy and the 120 s acquisition budget,
// checking interruption between bounded lock attempts.
[[nodiscard]] bool lockOcrCacheFile(QLockFile& lock,
                                    const std::function<bool()>& interruptionRequested);
} // namespace snow_shot::presentation::detail
