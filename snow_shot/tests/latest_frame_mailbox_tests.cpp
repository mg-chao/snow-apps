#include "presentation/capture/latestframemailbox.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {
std::optional<std::string> environmentVariable(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    const std::unique_ptr<char, decltype(&std::free)> ownedValue(value, &std::free);
    return std::string(ownedValue.get());
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

template <typename T> T takeRequired(std::optional<T> value, const char* message) {
    if (!value.has_value()) {
        std::cerr << message << '\n';
        std::exit(1);
    }
    return std::move(*value);
}

struct DestructionCounts {
    std::array<int, 16> values{};
};

class TrackedItem {
  public:
    TrackedItem(int identifier, std::shared_ptr<DestructionCounts> counts)
        : m_identifier(identifier), m_counts(std::move(counts)) {}

    ~TrackedItem() {
        if (m_counts != nullptr) {
            ++m_counts->values[static_cast<std::size_t>(m_identifier)];
        }
    }

    TrackedItem(const TrackedItem&) = delete;
    TrackedItem& operator=(const TrackedItem&) = delete;

    TrackedItem(TrackedItem&& other) noexcept
        : m_identifier(std::exchange(other.m_identifier, 0)), m_counts(std::move(other.m_counts)) {}

    TrackedItem& operator=(TrackedItem&& other) noexcept {
        if (this != &other) {
            if (m_counts != nullptr) {
                ++m_counts->values[static_cast<std::size_t>(m_identifier)];
            }
            m_identifier = std::exchange(other.m_identifier, 0);
            m_counts = std::move(other.m_counts);
        }
        return *this;
    }

    [[nodiscard]] int identifier() const {
        return m_identifier;
    }

  private:
    int m_identifier = 0;
    std::shared_ptr<DestructionCounts> m_counts;
};

using Mailbox = snow_shot::capture_detail::LatestFrameMailbox<TrackedItem, int>;

struct OverloadResult {
    int published = 0;
    int replacements = 0;
    int consumed = 0;
    int lastConsumed = 0;
};

OverloadResult simulateOverload() {
    using IntMailbox = snow_shot::capture_detail::LatestFrameMailbox<int, int>;
    IntMailbox mailbox;
    mailbox.reset(1);
    std::optional<IntMailbox::Entry> inFlight;
    OverloadResult result;
    for (int tick = 0; tick < 100; ++tick) {
        const auto publication = mailbox.publish(1, tick + 1);
        ++result.published;
        result.replacements += static_cast<int>(publication.replaced.has_value());
        require(mailbox.pendingDepth() == 1, "only the newest frame may remain pending");
        if (publication.wakeConsumer) {
            inFlight = mailbox.take();
        }
        if ((tick + 1) % 4 == 0 && inFlight.has_value()) {
            ++result.consumed;
            result.lastConsumed = inFlight->item;
            const bool hasNext = mailbox.finish(1);
            inFlight.reset();
            if (hasNext) {
                inFlight = mailbox.take();
                require(inFlight.has_value() && inFlight->item == tick + 1,
                        "each completed worker must receive the newest published frame");
            }
        }
    }
    while (inFlight.has_value()) {
        ++result.consumed;
        result.lastConsumed = inFlight->item;
        const bool hasNext = mailbox.finish(1);
        inFlight.reset();
        if (hasNext) {
            inFlight = mailbox.take();
        }
    }
    return result;
}

void replacesPendingBeforeDispatch() {
    auto counts = std::make_shared<DestructionCounts>();
    {
        Mailbox mailbox;
        mailbox.reset(7);
        require(mailbox.publish(7, TrackedItem(1, counts)).wakeConsumer, "first frame must wake");
        require(!mailbox.publish(7, TrackedItem(2, counts)).wakeConsumer,
                "replacement must reuse the outstanding wake");
        require(counts->values[1] == 1, "first pending frame must be released on replacement");
        require(!mailbox.publish(7, TrackedItem(3, counts)).wakeConsumer,
                "latest replacement must not wake again");
        require(mailbox.pendingDepth() == 1, "pending depth must remain one");
        require(counts->values[2] == 1, "second pending frame must be released on replacement");

        auto latest = takeRequired(mailbox.take(), "latest frame must be available");
        require(latest.item.identifier() == 3, "newest latest frame must be retained");
        require(mailbox.pendingDepth() == 0, "in-flight frame must not count as pending");
        require(!mailbox.finish(7), "empty mailbox must stop the consumer");
    }
    require(counts->values[1] == 1, "first frame must be destroyed exactly once");
    require(counts->values[2] == 1, "replaced frame must be destroyed exactly once");
    require(counts->values[3] == 1, "latest frame must be destroyed exactly once");
}

void replacesPendingWhileInFlight() {
    auto counts = std::make_shared<DestructionCounts>();
    {
        Mailbox mailbox;
        mailbox.reset(7);
        require(mailbox.publish(7, TrackedItem(1, counts)).wakeConsumer, "first frame must wake");
        auto inFlight = takeRequired(mailbox.take(), "first frame must enter flight");
        require(!mailbox.publish(7, TrackedItem(2, counts)).wakeConsumer,
                "pending frame must wait for the in-flight completion");
        require(!mailbox.publish(7, TrackedItem(3, counts)).wakeConsumer,
                "replacing a pending frame must not dispatch a second worker");
        require(counts->values[1] == 0 && counts->values[2] == 1,
                "replacement must release only the superseded pending frame");
        require(inFlight.item.identifier() == 1 && mailbox.pendingDepth() == 1,
                "one pending frame may coexist with the original in-flight frame");
        require(!mailbox.take().has_value(), "only one worker frame may be in flight");
        require(!mailbox.finish(8), "wrong-generation completion must preserve in-flight work");
        require(mailbox.finish(7), "completion must schedule the newest pending frame");
        require(!mailbox.finish(7), "completion must schedule the pending frame only once");
        require(!mailbox.publish(7, TrackedItem(4, counts)).wakeConsumer,
                "replacement between completion and dispatch must reuse the scheduled wake");
        require(counts->values[3] == 1, "replacement must release the frame awaiting dispatch");
        auto latest = takeRequired(mailbox.take(), "latest pending frame must enter flight");
        require(latest.item.identifier() == 4, "worker must take the latest replacement");
        require(!mailbox.finish(7), "last completion must make the mailbox idle");
        require(mailbox.publish(7, TrackedItem(5, counts)).wakeConsumer,
                "publication after draining must wake the idle consumer");
    }
    for (int identifier = 1; identifier <= 5; ++identifier) {
        require(counts->values[static_cast<std::size_t>(identifier)] == 1,
                "processed, replaced and cancelled frames must be destroyed exactly once");
    }
}

void resetRejectsStaleGeneration() {
    auto counts = std::make_shared<DestructionCounts>();
    {
        Mailbox mailbox;
        mailbox.reset(10);
        require(mailbox.publish(10, TrackedItem(4, counts)).wakeConsumer,
                "first generation must wake");
        auto inFlight = mailbox.take();
        require(inFlight.has_value(), "first generation frame must enter flight");

        require(!mailbox.publish(10, TrackedItem(7, counts)).wakeConsumer,
                "old generation must retain one pending frame");
        mailbox.reset(11);
        require(counts->values[7] == 1, "reset must immediately release the old pending frame");
        require(!mailbox.publish(10, TrackedItem(5, counts)).wakeConsumer,
                "stale generation must be rejected");
        require(counts->values[5] == 1, "rejected stale frame must be destroyed");
        require(!mailbox.finish(10), "stale completion must not alter new generation");
        require(mailbox.publish(11, TrackedItem(6, counts)).wakeConsumer,
                "new generation must wake");
        auto next = takeRequired(mailbox.take(), "new generation must own its in-flight frame");
        require(!mailbox.finish(10), "stale completion must not finish the new in-flight frame");
        require(next.item.identifier() == 6 && !mailbox.finish(11),
                "new generation must finish independently");
    }
    require(counts->values[4] == 1, "in-flight stale frame must be destroyed once");
    require(counts->values[5] == 1, "rejected stale frame must be destroyed once");
    require(counts->values[6] == 1, "new generation frame must be destroyed once");
}

void overloadKeepsNewestFrame() {
    const OverloadResult result = simulateOverload();
    require(result.published == 100 && result.replacements > 0,
            "overload must admit every frame and replace superseded pending frames");
    require(result.consumed + result.replacements == result.published,
            "every published frame must be processed or replaced");
    require(result.lastConsumed == 100, "draining must process the final published frame");

    if (const auto output = environmentVariable("SNOW_SCROLLING_PERF_OUTPUT")) {
        std::ofstream stream(*output, std::ios::trunc);
        stream << "{\n"
               << "  \"published\": " << result.published << ",\n"
               << "  \"replacements\": " << result.replacements << ",\n"
               << "  \"consumed\": " << result.consumed << ",\n"
               << "  \"last_consumed\": " << result.lastConsumed << "\n"
               << "}\n";
    }
}
} // namespace

int main() {
    replacesPendingBeforeDispatch();
    replacesPendingWhileInFlight();
    resetRejectsStaleGeneration();
    overloadKeepsNewestFrame();
    return 0;
}
