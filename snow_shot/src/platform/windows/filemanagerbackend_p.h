#ifndef SNOW_SHOT_PLATFORM_WINDOWS_FILEMANAGERBACKEND_P_H
#define SNOW_SHOT_PLATFORM_WINDOWS_FILEMANAGERBACKEND_P_H

struct IQueryAssociations;

namespace snow_shot::platform::detail {

// Also used with isolated association keys in tests; never changes system associations.
[[nodiscard]] bool isExplorerAssociation(IQueryAssociations* associations);

} // namespace snow_shot::platform::detail

#endif
