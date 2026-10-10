#pragma once

#include "snow/image/export.h"
#include "snow/image/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace snow::image {

class SNOW_IMAGE_API ByteSource {
  public:
    virtual ~ByteSource() = default;
    [[nodiscard]] virtual Result<std::uint64_t> size() const = 0;
    [[nodiscard]] virtual Result<std::size_t> read_at(std::uint64_t offset,
                                                      std::span<std::byte> destination) const = 0;
    // A stable, immutable view of the entire source, valid while this source is alive.
    // Sources without contiguous storage retain the ordinary read_at path.
    [[nodiscard]] virtual std::optional<std::span<const std::byte>> contiguous_bytes() const {
        return std::nullopt;
    }
};

class SNOW_IMAGE_API ByteSink {
  public:
    virtual ~ByteSink() = default;
    [[nodiscard]] virtual Result<void> write(std::span<const std::byte> source) = 0;
    [[nodiscard]] virtual Result<std::uint64_t> position() const = 0;
    [[nodiscard]] virtual Result<void> seek(std::uint64_t position) = 0;
    [[nodiscard]] virtual Result<void> flush() = 0;
    [[nodiscard]] virtual bool seekable() const noexcept = 0;
};

struct Input final {
    std::shared_ptr<const ByteSource> source;
    std::string name_hint;
};

// Retains the source of a borrowed view, or owns the bounded read buffer for a
// non-contiguous source. Computing the view on access keeps moves safe.
class InputBytes final {
  public:
    explicit InputBytes(std::vector<std::byte> bytes) : owned_(std::move(bytes)) {}
    InputBytes(std::shared_ptr<const ByteSource> source, std::span<const std::byte> bytes)
        : source_(std::move(source)), borrowed_(bytes) {}

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return source_ ? borrowed_ : std::span<const std::byte>(owned_);
    }
    [[nodiscard]] const std::byte* data() const noexcept {
        return bytes().data();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return bytes().size();
    }

  private:
    std::shared_ptr<const ByteSource> source_;
    std::vector<std::byte> owned_;
    std::span<const std::byte> borrowed_;
};

struct Output final {
    std::shared_ptr<ByteSink> sink;
    std::string name_hint;
};

SNOW_IMAGE_API Result<Input> file_input(const std::filesystem::path& path);
SNOW_IMAGE_API Result<Output> file_output(const std::filesystem::path& path);
SNOW_IMAGE_API Input memory_input(std::shared_ptr<const std::vector<std::byte>> bytes,
                                  std::string name_hint = {});
SNOW_IMAGE_API Input memory_input(std::span<const std::byte> bytes, std::string name_hint = {});
// Memory outputs are single-writer sinks. The capacity hint is applied before
// encoding and seek support is retained for container codecs.
SNOW_IMAGE_API Output memory_output(std::shared_ptr<std::vector<std::byte>> bytes,
                                    std::string name_hint = {}, std::size_t initial_capacity = 0);
SNOW_IMAGE_API Result<std::vector<std::byte>> read_all(const ByteSource& source,
                                                       std::uint64_t maximum_bytes);
// Borrow memory-backed inputs without an encoded-data copy. File and streaming
// inputs are read once into owned storage under the same byte limit.
SNOW_IMAGE_API Result<InputBytes> read_contiguous(const Input& input, std::uint64_t maximum_bytes);

} // namespace snow::image
