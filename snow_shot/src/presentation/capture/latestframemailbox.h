#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

namespace snow_shot::capture_detail {
// A single-consumer mailbox with one pending frame, replaced by every newer publication.
template <typename Item, typename Generation> class LatestFrameMailbox {
  public:
    struct Entry {
        Generation generation;
        Item item;
    };

    struct PublishResult {
        bool wakeConsumer = false;
        std::optional<Item> replaced;
    };

    void reset(Generation generation) {
        std::lock_guard lock(m_mutex);
        m_generation = generation;
        m_pending.reset();
        m_inFlightGeneration.reset();
    }

    [[nodiscard]] PublishResult publish(Generation generation, Item item) {
        std::lock_guard lock(m_mutex);
        if (generation != m_generation) {
            return {};
        }
        PublishResult result;
        result.wakeConsumer = !m_pending.has_value() && !m_inFlightGeneration.has_value();
        if (m_pending.has_value()) {
            result.replaced.emplace(std::move(m_pending->item));
        }
        m_pending.emplace(Entry{generation, std::move(item)});
        return result;
    }

    [[nodiscard]] std::optional<Entry> take() {
        std::lock_guard lock(m_mutex);
        if (m_inFlightGeneration.has_value() || !m_pending.has_value()) {
            return std::nullopt;
        }
        std::optional<Entry> result = std::move(m_pending);
        m_pending.reset();
        m_inFlightGeneration = result->generation;
        return result;
    }

    // Returns true exactly once when a completed consumer must take the next
    // latest pending frame.
    [[nodiscard]] bool finish(Generation generation) {
        std::lock_guard lock(m_mutex);
        if (!m_inFlightGeneration.has_value() || generation != *m_inFlightGeneration) {
            return false;
        }
        m_inFlightGeneration.reset();
        return m_pending.has_value();
    }

    [[nodiscard]] std::size_t pendingDepth() const {
        std::lock_guard lock(m_mutex);
        return static_cast<std::size_t>(m_pending.has_value());
    }

  private:
    mutable std::mutex m_mutex;
    Generation m_generation{};
    std::optional<Generation> m_inFlightGeneration;
    std::optional<Entry> m_pending;
};
} // namespace snow_shot::capture_detail
