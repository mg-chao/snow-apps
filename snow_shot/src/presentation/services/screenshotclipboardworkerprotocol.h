#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDWORKERPROTOCOL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDWORKERPROTOCOL_H

#include <cstdint>

namespace snow_shot::presentation::clipboard_worker_protocol {

constexpr std::uint32_t kRequestMagic = 0x53434252;
constexpr std::uint32_t kResponseMagic = 0x53434253;
constexpr std::uint32_t kVersion = 1;
constexpr std::uint64_t kMaximumPayloadBytes = 512ull * 1024ull * 1024ull;

enum class Status : std::uint32_t {
    Success = 0,
    InvalidPayload = 1,
    ClipboardUnavailable = 2,
    Busy = 3,
    ClearFailed = 4,
    PublishFailed = 5,
};

#pragma pack(push, 1)
struct RequestHeader final {
    std::uint32_t magic = kRequestMagic;
    std::uint32_t version = kVersion;
    std::uint32_t nativeFormat = 0;
    std::uint32_t reserved = 0;
    std::uint64_t payloadBytes = 0;
};

struct ResponseHeader final {
    std::uint32_t magic = kResponseMagic;
    std::uint32_t version = kVersion;
    std::uint32_t status = static_cast<std::uint32_t>(Status::InvalidPayload);
    std::uint32_t nativeError = 0;
    std::uint32_t attempts = 0;
    std::uint32_t reserved = 0;
};
#pragma pack(pop)

static_assert(sizeof(RequestHeader) == 24);
static_assert(sizeof(ResponseHeader) == 24);

} // namespace snow_shot::presentation::clipboard_worker_protocol

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDWORKERPROTOCOL_H
