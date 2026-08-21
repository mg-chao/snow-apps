#include "presentation/capture/latestbridgemailbox.h"

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

using Mailbox = snow_shot::capture_detail::LatestBridgeMailbox<TrackedItem, int>;

struct OverloadResult {
    int executed = 0;
    int replacements = 0;
    int consumed = 0;
};

OverloadResult simulateOverload(bool backpressure) {
    using IntMailbox = snow_shot::capture_detail::LatestBridgeMailbox<int, int>;
    IntMailbox mailbox;
    mailbox.reset(1);
    std::optional<IntMailbox::Entry> inFlight;
    OverloadResult result;
    for (int tick = 0; tick < 100; ++tick) {
        if (!backpressure || mailbox.hasPendingCapacity()) {
            if (mailbox.pendingDepth() == 2) {
                ++result.replacements;
            }
            const bool wake = mailbox.publish(1, tick + 1);
            ++result.executed;
            if (wake) {
                inFlight = mailbox.take();
            }
        }
        if ((tick + 1) % 4 == 0 && inFlight.has_value()) {
            ++result.consumed;
            const bool hasNext = mailbox.finish(1);
            inFlight.reset();
            if (hasNext) {
                inFlight = mailbox.take();
            }
        }
    }
    while (inFlight.has_value()) {
        ++result.consumed;
        const bool hasNext = mailbox.finish(1);
        inFlight.reset();
        if (hasNext) {
            inFlight = mailbox.take();
        }
    }
    return result;
}

void preservesBridgeAndReplacesLatest() {
    auto counts = std::make_shared<DestructionCounts>();
    {
        Mailbox mailbox;
        mailbox.reset(7);
        require(mailbox.publish(7, TrackedItem(1, counts)), "first frame must wake");
        require(!mailbox.publish(7, TrackedItem(2, counts)),
                "second frame must not add another wake");
        require(mailbox.pendingDepth() == 2, "bridge/latest depth must be two");
        require(!mailbox.publish(7, TrackedItem(3, counts)), "latest replacement must not wake");
        require(mailbox.pendingDepth() == 2, "pending depth must remain bounded");
        require(counts->values[2] == 1, "replaced latest frame must be destroyed");

        auto bridge = takeRequired(mailbox.take(), "bridge frame must be available");
        require(bridge.item.identifier() == 1, "bridge frame must never be overwritten");
        require(mailbox.pendingDepth() == 1, "latest frame must promote to bridge");
        require(mailbox.finish(7), "finish must schedule the promoted bridge once");
        require(!mailbox.finish(7), "finish must not schedule the same bridge twice");

        auto latest = takeRequired(mailbox.take(), "promoted latest frame must be available");
        require(latest.item.identifier() == 3, "newest latest frame must be retained");
        require(!mailbox.finish(7), "empty mailbox must stop the consumer");
    }
    require(counts->values[1] == 1, "bridge frame must be destroyed exactly once");
    require(counts->values[2] == 1, "replaced frame must be destroyed exactly once");
    require(counts->values[3] == 1, "latest frame must be destroyed exactly once");
}

void resetRejectsStaleGeneration() {
    auto counts = std::make_shared<DestructionCounts>();
    {
        Mailbox mailbox;
        mailbox.reset(10);
        require(mailbox.publish(10, TrackedItem(4, counts)), "first generation must wake");
        auto inFlight = mailbox.take();
        require(inFlight.has_value(), "first generation frame must enter flight");

        mailbox.reset(11);
        require(!mailbox.publish(10, TrackedItem(5, counts)), "stale generation must be rejected");
        require(counts->values[5] == 1, "rejected stale frame must be destroyed");
        require(!mailbox.finish(10), "stale completion must not alter new generation");
        require(mailbox.publish(11, TrackedItem(6, counts)), "new generation must wake");
        require(mailbox.pendingDepth() == 1, "new generation must own one bridge");
    }
    require(counts->values[4] == 1, "in-flight stale frame must be destroyed once");
    require(counts->values[5] == 1, "rejected stale frame must be destroyed once");
    require(counts->values[6] == 1, "new bridge frame must be destroyed once");
}

void backpressureAvoidsGuaranteedReplacementWork() {
    const OverloadResult baseline = simulateOverload(false);
    const OverloadResult optimized = simulateOverload(true);
    require(baseline.executed == 100, "baseline must execute every producer tick");
    require(baseline.replacements > 0, "baseline must replace latest frames under load");
    require(optimized.replacements == 0, "backpressure must avoid latest replacement");
    require(optimized.executed * 2 <= baseline.executed,
            "backpressure must eliminate at least half of overloaded captures");
    require(optimized.consumed > 0, "backpressure must continue feeding the consumer");

    if (const auto output = environmentVariable("SNOW_SCROLLING_PERF_OUTPUT")) {
        std::ofstream stream(*output, std::ios::trunc);
        stream << "{\n"
               << "  \"baseline_executed\": " << baseline.executed << ",\n"
               << "  \"baseline_replacements\": " << baseline.replacements << ",\n"
               << "  \"optimized_executed\": " << optimized.executed << ",\n"
               << "  \"optimized_replacements\": " << optimized.replacements << ",\n"
               << "  \"optimized_consumed\": " << optimized.consumed << "\n"
               << "}\n";
    }
}

void completionReportsPendingAndReplacementPressure() {
    Mailbox mailbox;
    mailbox.reset(3);
    require(mailbox.publish(3, TrackedItem(7, std::make_shared<DestructionCounts>())),
            "pressure test bridge must wake");
    auto inFlight = mailbox.take();
    require(inFlight.has_value(), "pressure test frame must enter flight");
    auto counts = std::make_shared<DestructionCounts>();
    require(!mailbox.publish(3, TrackedItem(8, counts)), "pressure test pending frame must coalesce");
    require(!mailbox.publish(3, TrackedItem(9, counts)), "pressure test latest frame must replace");
    require(!mailbox.publish(3, TrackedItem(10, counts)), "pressure test latest frame must replace again");
    const auto completion = mailbox.finishWithFeedback(3);
    require(completion.hasNext, "completion must preserve the pending bridge");
    require(completion.pendingDepth == 2,
            "completion must report bridge plus latest pending depth");
    require(completion.replacedFrames == 2, "completion must report two replacements");
    require(counts->values[9] == 1, "replaced frame must be destroyed");
}
} // namespace

int main() {
    preservesBridgeAndReplacesLatest();
    resetRejectsStaleGeneration();
    backpressureAvoidsGuaranteedReplacementWork();
    completionReportsPendingAndReplacementPressure();
    return 0;
}
