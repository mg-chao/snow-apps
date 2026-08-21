#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

namespace snow_shot::capture_detail {
// A single-consumer mailbox that preserves the next bridge frame while
// coalescing all additional producer updates into one replaceable latest frame.
template <typename Item, typename Generation> class LatestBridgeMailbox {
  public:
    struct Completion {
        bool hasNext = false;
        std::size_t pendingDepth = 0;
        std::size_t replacedFrames = 0;
    };

    struct Entry {
        Generation generation;
        Item item;
    };

    void reset(Generation generation) {
        std::lock_guard lock(m_mutex);
        m_generation = generation;
        m_bridge.reset();
        m_latest.reset();
        m_inFlightGeneration.reset();
        m_consumerActive = false;
        m_replacedFrames = 0;
    }

    [[nodiscard]] bool publish(Generation generation, Item item) {
        std::lock_guard lock(m_mutex);
        if (generation != m_generation) {
            return false;
        }
        if (!m_consumerActive) {
            m_bridge.emplace(Entry{generation, std::move(item)});
            m_consumerActive = true;
            return true;
        }
        if (!m_bridge.has_value()) {
            m_bridge.emplace(Entry{generation, std::move(item)});
        } else {
            ++m_replacedFrames;
            m_latest.emplace(Entry{generation, std::move(item)});
        }
        return false;
    }

    [[nodiscard]] std::optional<Entry> take() {
        std::lock_guard lock(m_mutex);
        if (m_inFlightGeneration.has_value() || !m_bridge.has_value()) {
            if (!m_inFlightGeneration.has_value()) {
                m_consumerActive = false;
            }
            return std::nullopt;
        }
        std::optional<Entry> result = std::move(m_bridge);
        m_bridge.reset();
        if (m_latest.has_value()) {
            m_bridge.emplace(std::move(*m_latest));
            m_latest.reset();
        }
        m_inFlightGeneration = result->generation;
        return result;
    }

    // Returns true exactly once when a completed consumer must take the next
    // preserved bridge frame.
    [[nodiscard]] bool finish(Generation generation) {
        return finishWithFeedback(generation).hasNext;
    }

    // Completes the in-flight item and snapshots pressure accumulated while it
    // was processed. pendingDepth includes both the preserved bridge and the
    // replaceable latest item, before the next consumer take promotes either.
    [[nodiscard]] Completion finishWithFeedback(Generation generation) {
        std::lock_guard lock(m_mutex);
        if (!m_inFlightGeneration.has_value() || generation != *m_inFlightGeneration) {
            return {};
        }
        m_inFlightGeneration.reset();
        Completion completion;
        completion.replacedFrames = std::exchange(m_replacedFrames, 0);
        if (m_bridge.has_value()) {
            completion.hasNext = true;
            completion.pendingDepth = static_cast<std::size_t>(m_bridge.has_value()) +
                                       static_cast<std::size_t>(m_latest.has_value());
            return completion;
        }
        m_consumerActive = false;
        return completion;
    }

    [[nodiscard]] std::size_t pendingDepth() const {
        std::lock_guard lock(m_mutex);
        return static_cast<std::size_t>(m_bridge.has_value()) +
               static_cast<std::size_t>(m_latest.has_value());
    }

    [[nodiscard]] bool hasPendingCapacity(std::size_t capacity = 2) const {
        std::lock_guard lock(m_mutex);
        const std::size_t depth = static_cast<std::size_t>(m_bridge.has_value()) +
                                  static_cast<std::size_t>(m_latest.has_value());
        return depth < capacity;
    }

  private:
    mutable std::mutex m_mutex;
    Generation m_generation{};
    std::optional<Generation> m_inFlightGeneration;
    std::optional<Entry> m_bridge;
    std::optional<Entry> m_latest;
    bool m_consumerActive = false;
    std::size_t m_replacedFrames = 0;
};
} // namespace snow_shot::capture_detail
