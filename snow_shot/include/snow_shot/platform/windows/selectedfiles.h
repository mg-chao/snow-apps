#ifndef SNOW_SHOT_PLATFORM_WINDOWS_SELECTEDFILES_H
#define SNOW_SHOT_PLATFORM_WINDOWS_SELECTEDFILES_H

#include <QStringList>
#include <functional>
#include <memory>

namespace snow_shot::platform::windows {

struct SelectedFileTarget {
    quintptr window = 0;
    quintptr view = 0;
    quintptr tab = 0;
    quint32 processId = 0;
    bool desktop = false;
};

class SelectedFileBackend {
  public:
    virtual ~SelectedFileBackend() = default;
    // Capture the foreground identity on invocation; enumerate on a worker.
    [[nodiscard]] virtual SelectedFileTarget captureTarget() const = 0;
    [[nodiscard]] virtual QStringList
    selectedFiles(const SelectedFileTarget& target,
                  const std::function<bool()>& cancelled) const = 0;
};

[[nodiscard]] std::shared_ptr<SelectedFileBackend> createSelectedFileBackend();

} // namespace snow_shot::platform::windows
#endif
