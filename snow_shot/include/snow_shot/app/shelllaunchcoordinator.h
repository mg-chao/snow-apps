// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStringList>

#include <functional>
#include <utility>

namespace snow_shot::app {

// GUI-thread owner of accepted launch intent while storage or the updater holds admission.
// Inspection does not reserve capacity: the transport owns receipts, and the updater owns
// durable transfer/acknowledgement. Accepted paths preserve invocation order, including repeats.
class ShellLaunchCoordinator final {
  public:
    struct Request {
        bool filesRequested = false;
        bool valid = true;
        bool autostart = false;
        bool foreground = false;
        QStringList paths;
        QStringList unsupportedPaths;
    };
    struct Intent {
        QStringList paths;
        bool foreground = false;

        // Normal replay arguments omit updater-private flags. An empty intent is not a launch.
        [[nodiscard]] QStringList replayArguments() const;
    };
    using UpdateAdmission = std::function<bool(const Intent&)>;

    [[nodiscard]] static Request parse(const QStringList& arguments);
    [[nodiscard]] bool acceptsUpdate(const Request& request, bool accepting,
                                     const UpdateAdmission& admission) const;
    [[nodiscard]] Intent retainUpdate(const Request& request);
    void retainStorage(const Request& request);
    // Taking transfers ownership before replay, so reentrant input belongs to the next batch.
    [[nodiscard]] Intent takeUpdate();
    [[nodiscard]] Intent takeStorage();

    // The startup continuation is transferred only when a real file presentation owns it.
    // Invalid, unsupported and deferred launches settle it on return, leaving DeferredStartup
    // to observe current suspension before running background work. Completion must not throw.
    class FirstFileCompletion final {
      public:
        explicit FirstFileCompletion(std::function<void()> completion)
            : m_completion(std::move(completion)) {}
        FirstFileCompletion(const FirstFileCompletion&) = delete;
        FirstFileCompletion& operator=(const FirstFileCompletion&) = delete;
        ~FirstFileCompletion() {
            if (m_completion)
                m_completion();
        }
        [[nodiscard]] std::function<void()> take() {
            return std::exchange(m_completion, {});
        }

      private:
        std::function<void()> m_completion;
    };

  private:
    [[nodiscard]] Intent updateCandidate(const Request& request) const;
    Intent m_update;
    Intent m_storage;
};

} // namespace snow_shot::app
