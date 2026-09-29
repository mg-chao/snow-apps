#pragma once

#include "snow_shot/presentation/screenshotscrollingsnapshot.h"

#include <functional>
#include <memory>
#include <utility>

namespace snow_shot::capture_detail {
// Capture owns cancellation until export detaches the accepted request. The
// completion then owns its lifetime, independently of capture stop/restart.
// Requests and completions run on the capture controller's thread.
class ScrollingSnapshotRequest {
  public:
    using Completion = std::function<void(ScreenshotScrollingSnapshot)>;

    ScrollingSnapshotRequest() = default;
    ScrollingSnapshotRequest(const ScrollingSnapshotRequest&) = delete;
    ScrollingSnapshotRequest& operator=(const ScrollingSnapshotRequest&) = delete;

    ~ScrollingSnapshotRequest() {
        cancel();
    }

    bool pending() const {
        return m_pending && *m_pending;
    }

    Completion begin(Completion completion) {
        if (pending() || !completion)
            return {};
        m_pending = std::make_shared<bool>(true);
        return [accepted = m_pending,
                completion = std::move(completion)](ScreenshotScrollingSnapshot snapshot) mutable {
            if (std::exchange(*accepted, false))
                completion(std::move(snapshot));
        };
    }

    void cancel() {
        if (m_pending)
            *m_pending = false;
        m_pending.reset();
    }

    void detach() {
        m_pending.reset();
    }

  private:
    std::shared_ptr<bool> m_pending;
};
} // namespace snow_shot::capture_detail
