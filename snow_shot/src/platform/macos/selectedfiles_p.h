#ifndef SNOW_SHOT_PLATFORM_MACOS_SELECTEDFILES_P_H
#define SNOW_SHOT_PLATFORM_MACOS_SELECTEDFILES_P_H

#include "snow_shot/platform/selectedfiles.h"
#include <ApplicationServices/ApplicationServices.h>

namespace snow_shot::platform::macos {
// The transport seam keeps native descriptor construction and parsing under test
// without contacting Finder or displaying a privacy prompt.
class FinderEventTransport {
  public:
    virtual ~FinderEventTransport() = default;
    virtual quint32 finderProcessId() const = 0;
    virtual bool isFinderRunning(quint32 processId) const = 0;
    virtual OSStatus requestPermission(const AEAddressDesc& address) const = 0;
    virtual OSStatus send(const AppleEvent& event, AppleEvent& reply) const = 0;
};
std::shared_ptr<SelectedFileBackend>
createSelectedFileBackend(std::shared_ptr<FinderEventTransport> transport);
} // namespace snow_shot::platform::macos
#endif
