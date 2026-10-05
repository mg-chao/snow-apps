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
    using Cancellation = std::function<void()>;

    ScrollingSnapshotRequest() = default;
    ScrollingSnapshotRequest(const ScrollingSnapshotRequest&) = delete;
    ScrollingSnapshotRequest& operator=(const ScrollingSnapshotRequest&) = delete;

    ~ScrollingSnapshotRequest() {
        cancel();
    }

    bool pending() const {
        return m_pending && m_pending->pending;
    }

    Completion begin(Completion completion, Cancellation cancelled = {}) {
        if (pending() || !completion)
            return {};
        m_pending =
            std::make_shared<State>(State{true, std::move(completion), std::move(cancelled)});
        return [accepted = m_pending](ScreenshotScrollingSnapshot snapshot) {
            if (!std::exchange(accepted->pending, false))
                return;
            auto completion = std::move(accepted->completion);
            accepted->cancelled = {};
            completion(std::move(snapshot));
        };
    }

    void cancel() {
        auto accepted = std::exchange(m_pending, {});
        if (!accepted || !std::exchange(accepted->pending, false))
            return;
        auto cancelled = std::move(accepted->cancelled);
        accepted->completion = {};
        if (cancelled)
            cancelled();
    }

    void detach() {
        m_pending.reset();
    }

  private:
    struct State {
        bool pending;
        Completion completion;
        Cancellation cancelled;
    };
    std::shared_ptr<State> m_pending;
};
} // namespace snow_shot::capture_detail
