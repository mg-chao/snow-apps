#include "snow/image/io.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>

namespace snow::image {
namespace {

class FileSource final : public ByteSource {
  public:
    explicit FileSource(const std::filesystem::path& path) : stream_(path, std::ios::binary) {
        if (!stream_.is_open())
            return;
        stream_.seekg(0, std::ios::end);
        const std::streampos end = stream_.tellg();
        if (end < 0)
            return;
        size_ = static_cast<std::uint64_t>(end);
        stream_.clear();
        stream_.seekg(0, std::ios::beg);
        opened_ = stream_.good();
    }

    [[nodiscard]] bool opened() const noexcept {
        return opened_;
    }

    Result<std::uint64_t> size() const override {
        return size_;
    }

    Result<std::size_t> read_at(std::uint64_t offset,
                                std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return std::size_t{0};
        }
        if (offset >= size_)
            return std::size_t{0};
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return Status::error(ErrorCode::limit_exceeded, "File read exceeds stream limits.");
        }
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
            std::min<std::uint64_t>(destination.size(), size_ - offset),
            static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())));
        std::lock_guard lock(mutex_);
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream_) {
            return Status::error(ErrorCode::io_error, "Could not seek in the input file.");
        }
        stream_.read(reinterpret_cast<char*>(destination.data()),
                     static_cast<std::streamsize>(count));
        const std::streamsize read = stream_.gcount();
        if (read < 0 || stream_.bad() || (stream_.fail() && !stream_.eof())) {
            return Status::error(ErrorCode::io_error, "Could not read the input file.");
        }
        return static_cast<std::size_t>(read);
    }

  private:
    mutable std::ifstream stream_;
    std::uint64_t size_ = 0;
    bool opened_ = false;
    mutable std::mutex mutex_;
};

class FileSink final : public ByteSink {
  public:
    explicit FileSink(const std::filesystem::path& path)
        : stream_(path, std::ios::binary | std::ios::trunc) {}

    [[nodiscard]] bool opened() const noexcept {
        return stream_.is_open();
    }

    Result<void> write(std::span<const std::byte> source) override {
        std::lock_guard lock(mutex_);
        if (source.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            return Status::error(ErrorCode::limit_exceeded, "File write exceeds stream limits.");
        }
        stream_.write(reinterpret_cast<const char*>(source.data()),
                      static_cast<std::streamsize>(source.size()));
        if (!stream_) {
            return Status::error(ErrorCode::io_error, "Could not write the output file.");
        }
        return {};
    }

    Result<std::uint64_t> position() const override {
        std::lock_guard lock(mutex_);
        const std::streampos value = stream_.tellp();
        if (value < 0) {
            return Status::error(ErrorCode::io_error, "Could not query the output position.");
        }
        return static_cast<std::uint64_t>(value);
    }

    Result<void> seek(std::uint64_t position_value) override {
        if (position_value >
            static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return Status::error(ErrorCode::limit_exceeded, "Output seek exceeds stream limits.");
        }
        std::lock_guard lock(mutex_);
        stream_.seekp(static_cast<std::streamoff>(position_value));
        if (!stream_) {
            return Status::error(ErrorCode::io_error, "Could not seek in the output file.");
        }
        return {};
    }

    Result<void> flush() override {
        std::lock_guard lock(mutex_);
        stream_.flush();
        if (!stream_) {
            return Status::error(ErrorCode::io_error, "Could not flush the output file.");
        }
        return {};
    }

    bool seekable() const noexcept override {
        return true;
    }

  private:
    mutable std::ofstream stream_;
    mutable std::mutex mutex_;
};

class MemorySource final : public ByteSource {
  public:
    explicit MemorySource(std::shared_ptr<const std::vector<std::byte>> bytes)
        : bytes_(std::move(bytes)) {}

    Result<std::uint64_t> size() const override {
        return static_cast<std::uint64_t>(bytes_->size());
    }

    std::optional<std::span<const std::byte>> contiguous_bytes() const override {
        return std::span<const std::byte>(*bytes_);
    }

    Result<std::size_t> read_at(std::uint64_t offset,
                                std::span<std::byte> destination) const override {
        if (offset >= bytes_->size()) {
            return std::size_t{0};
        }
        const std::size_t available = bytes_->size() - static_cast<std::size_t>(offset);
        const std::size_t count = std::min(available, destination.size());
        std::copy_n(bytes_->data() + static_cast<std::size_t>(offset), count, destination.data());
        return count;
    }

  private:
    std::shared_ptr<const std::vector<std::byte>> bytes_;
};

class MemorySink final : public ByteSink {
  public:
    MemorySink(std::shared_ptr<std::vector<std::byte>> bytes, std::size_t initial_capacity)
        : bytes_(std::move(bytes)) {
        bytes_->clear();
        try {
            if (initial_capacity > 0)
                bytes_->reserve(initial_capacity);
        } catch (const std::bad_alloc&) {
            reserve_failed_ = true;
        }
    }

    Result<void> write(std::span<const std::byte> source) override {
        if (reserve_failed_) {
            return Status::error(ErrorCode::out_of_memory, "Could not reserve the memory output.");
        }
        if (position_ > std::numeric_limits<std::size_t>::max() - source.size()) {
            return Status::error(ErrorCode::limit_exceeded, "Memory output size overflows.");
        }
        const std::size_t end = position_ + source.size();
        try {
            if (end > bytes_->size()) {
                bytes_->resize(end);
            }
        } catch (const std::bad_alloc&) {
            return Status::error(ErrorCode::out_of_memory, "Could not grow the memory output.");
        }
        std::copy(source.begin(), source.end(),
                  bytes_->begin() + static_cast<std::ptrdiff_t>(position_));
        position_ = end;
        return {};
    }

    Result<std::uint64_t> position() const override {
        return static_cast<std::uint64_t>(position_);
    }

    Result<void> seek(std::uint64_t position_value) override {
        if (position_value > std::numeric_limits<std::size_t>::max()) {
            return Status::error(ErrorCode::limit_exceeded, "Memory output seek exceeds limits.");
        }
        position_ = static_cast<std::size_t>(position_value);
        return {};
    }

    Result<void> flush() override {
        return {};
    }
    bool seekable() const noexcept override {
        return true;
    }

  private:
    std::shared_ptr<std::vector<std::byte>> bytes_;
    std::size_t position_ = 0;
    bool reserve_failed_ = false;
};

} // namespace

Result<Input> file_input(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return Status::error(ErrorCode::io_error,
                             error ? error.message() : "Input path is not a regular file.");
    }
    try {
        const std::u8string utf8_name = path.filename().u8string();
        auto source = std::make_shared<FileSource>(path);
        if (!source->opened()) {
            return Status::error(ErrorCode::io_error, "Could not open the input file.");
        }
        return Input{std::move(source), std::string(utf8_name.begin(), utf8_name.end())};
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not create the file input.");
    } catch (const std::filesystem::filesystem_error& filesystemError) {
        return Status::error(ErrorCode::io_error, filesystemError.what());
    }
}

Result<Output> file_output(const std::filesystem::path& path) {
    try {
        auto sink = std::make_shared<FileSink>(path);
        if (!sink->opened()) {
            return Status::error(ErrorCode::io_error, "Could not open the output file.");
        }
        return Output{std::move(sink), path.filename().string()};
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not create the file output.");
    }
}

Input memory_input(std::shared_ptr<const std::vector<std::byte>> bytes, std::string name_hint) {
    if (!bytes) {
        bytes = std::make_shared<const std::vector<std::byte>>();
    }
    return {std::make_shared<MemorySource>(std::move(bytes)), std::move(name_hint)};
}

Input memory_input(std::span<const std::byte> bytes, std::string name_hint) {
    auto copy = std::make_shared<std::vector<std::byte>>(bytes.begin(), bytes.end());
    return memory_input(std::move(copy), std::move(name_hint));
}

Output memory_output(std::shared_ptr<std::vector<std::byte>> bytes, std::string name_hint,
                     std::size_t initial_capacity) {
    if (!bytes) {
        bytes = std::make_shared<std::vector<std::byte>>();
    }
    return {std::make_shared<MemorySink>(std::move(bytes), initial_capacity), std::move(name_hint)};
}

Result<std::vector<std::byte>> read_all(const ByteSource& source, std::uint64_t maximum_bytes) {
    const Result<std::uint64_t> source_size = source.size();
    if (!source_size) {
        return source_size.error();
    }
    if (source_size.value() > maximum_bytes ||
        source_size.value() > std::numeric_limits<std::size_t>::max()) {
        return Status::error(ErrorCode::limit_exceeded, "Input exceeds the configured byte limit.");
    }
    try {
        std::vector<std::byte> bytes(static_cast<std::size_t>(source_size.value()));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            Result<std::size_t> count = source.read_at(offset, std::span(bytes).subspan(offset));
            if (!count) {
                return count.error();
            }
            if (count.value() == 0) {
                return Status::error(ErrorCode::truncated_data,
                                     "Input ended before its reported size.");
            }
            offset += count.value();
        }
        return bytes;
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate the input buffer.");
    }
}

Result<InputBytes> read_contiguous(const Input& input, std::uint64_t maximum_bytes) {
    if (!input.source)
        return Status::error(ErrorCode::invalid_argument, "Input has no byte source.");
    const auto source_size = input.source->size();
    if (!source_size)
        return source_size.error();
    if (source_size.value() > maximum_bytes ||
        source_size.value() > std::numeric_limits<std::size_t>::max()) {
        return Status::error(ErrorCode::limit_exceeded, "Input exceeds the configured byte limit.");
    }
    if (const auto view = input.source->contiguous_bytes()) {
        if (view->size() != source_size.value()) {
            return Status::error(ErrorCode::io_error,
                                 "Contiguous input does not match its reported size.");
        }
        return InputBytes(input.source, *view);
    }
    auto owned = read_all(*input.source, maximum_bytes);
    if (!owned)
        return owned.error();
    return InputBytes(std::move(owned).value());
}

} // namespace snow::image
