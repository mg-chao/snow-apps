#include "snow/image/io.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
using namespace snow::image;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class ChunkedSource final : public ByteSource {
  public:
    explicit ChunkedSource(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}
    Result<std::uint64_t> size() const override {
        return bytes_.size() + (truncated ? 1 : 0);
    }
    Result<std::size_t> read_at(std::uint64_t offset,
                                std::span<std::byte> destination) const override {
        if (failed)
            return Status::error(ErrorCode::io_error, "Fixture read failure.");
        if (offset >= bytes_.size())
            return std::size_t{0};
        const auto count = std::min(
            {std::size_t{3}, destination.size(), bytes_.size() - static_cast<std::size_t>(offset)});
        std::copy_n(bytes_.data() + offset, count, destination.data());
        return count;
    }
    bool truncated = false;
    bool failed = false;

  private:
    std::vector<std::byte> bytes_;
};

class InvalidViewSource final : public ByteSource {
  public:
    Result<std::uint64_t> size() const override {
        return 1;
    }
    Result<std::size_t> read_at(std::uint64_t, std::span<std::byte>) const override {
        throw std::runtime_error("A borrowed source must not be read into another buffer.");
    }
    std::optional<std::span<const std::byte>> contiguous_bytes() const override {
        return std::span<const std::byte>{};
    }
};

void borrowedInputRetainsItsOwnerWithoutCopying() {
    auto payload = std::make_shared<const std::vector<std::byte>>(
        std::initializer_list<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}});
    const std::weak_ptr<const std::vector<std::byte>> lifetime(payload);
    const auto* original = payload->data();
    Input input = memory_input(payload);
    auto read = read_contiguous(input, payload->size());
    require(read && read.value().data() == original,
            "Memory-backed input must borrow the original encoded allocation.");
    std::optional<InputBytes> retained(std::move(read).value());
    input = {};
    payload.reset();
    require(!lifetime.expired() && retained->bytes()[2] == std::byte{3},
            "The borrowed bytes must keep their source alive after the Input is destroyed.");
    retained.reset();
    require(lifetime.expired(), "The final borrowed owner must release the encoded allocation.");
}

void nonContiguousInputOwnsOneCompleteRead() {
    const std::vector<std::byte> expected{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                          std::byte{5}, std::byte{6}, std::byte{7}};
    auto source = std::make_shared<ChunkedSource>(expected);
    const std::weak_ptr<ChunkedSource> lifetime(source);
    Input input{source, {}};
    auto read = read_contiguous(input, expected.size());
    require(read && std::ranges::equal(read.value().bytes(), expected),
            "A chunked source must produce a complete contiguous encoded buffer.");
    auto retained = std::move(read).value();
    input = {};
    source.reset();
    require(lifetime.expired() && std::ranges::equal(retained.bytes(), expected),
            "Owned input bytes must be independent of the original source and survive moves.");
}

void limitsAndReadFailuresRemainObservable() {
    const std::array<std::byte, 2> bytes{std::byte{1}, std::byte{2}};
    const auto memory = memory_input(bytes);
    const auto oversized = read_contiguous(memory, 1);
    require(!oversized && oversized.error().code == ErrorCode::limit_exceeded,
            "Borrowing must enforce the same encoded byte limit as an owned read.");
    auto source =
        std::make_shared<ChunkedSource>(std::vector<std::byte>(bytes.begin(), bytes.end()));
    const auto ownedOversized = read_contiguous(Input{source, {}}, 1);
    require(!ownedOversized && ownedOversized.error().code == ErrorCode::limit_exceeded,
            "Non-contiguous input must enforce the encoded byte limit.");
    source->truncated = true;
    const auto truncated = read_contiguous(Input{source, {}}, 3);
    require(!truncated && truncated.error().code == ErrorCode::truncated_data,
            "Partial input must not be published as a complete encoded buffer.");
    source->failed = true;
    const auto failed = read_contiguous(Input{source, {}}, 3);
    require(!failed && failed.error().code == ErrorCode::io_error,
            "Source failures must propagate through the contiguous input adapter.");
    const auto invalid = read_contiguous(Input{std::make_shared<InvalidViewSource>(), {}}, 1);
    require(!invalid && invalid.error().code == ErrorCode::io_error,
            "A borrowed view must match the source's reported size.");
    const auto missing = read_contiguous({}, 1);
    require(!missing && missing.error().code == ErrorCode::invalid_argument,
            "Missing byte sources must be rejected.");
    const auto empty = read_contiguous(memory_input(std::span<const std::byte>{}), 0);
    require(empty && empty.value().size() == 0, "Empty memory-backed inputs must remain valid.");
}
} // namespace

int main() {
    try {
        borrowedInputRetainsItsOwnerWithoutCopying();
        nonContiguousInputOwnsOneCompleteRead();
        limitsAndReadFailuresRemainObservable();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
