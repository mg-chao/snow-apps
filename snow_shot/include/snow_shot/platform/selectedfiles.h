#ifndef SNOW_SHOT_PLATFORM_SELECTEDFILES_H
#define SNOW_SHOT_PLATFORM_SELECTEDFILES_H

#include <QStringList>
#include <functional>
#include <memory>

namespace snow_shot::platform {

struct SelectedFileTarget {
    quintptr window = 0;
    quintptr view = 0;
    quintptr tab = 0;
    quint32 processId = 0;
    bool desktop = false;
};

enum class SelectedFileError { None, PermissionDenied, Timeout, Unavailable, QueryFailed };

struct SelectedFileResult {
    QStringList paths;
    SelectedFileError error = SelectedFileError::None;
};

class SelectedFileBackend {
  public:
    virtual ~SelectedFileBackend() = default;
    // Capture the source identity on invocation; enumerate on a worker.
    // Windows uses the foreground shell view; macOS uses the running Finder.
    [[nodiscard]] virtual SelectedFileTarget captureTarget() const = 0;
    [[nodiscard]] virtual bool isValidTarget(const SelectedFileTarget& target) const {
        return target.window != 0 && target.view != 0;
    }
    [[nodiscard]] virtual SelectedFileResult
    selectedFiles(const SelectedFileTarget& target,
                  const std::function<bool()>& cancelled) const = 0;
};

[[nodiscard]] std::shared_ptr<SelectedFileBackend> createSelectedFileBackend();

} // namespace snow_shot::platform
#endif
