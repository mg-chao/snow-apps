#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRWORKERPROTOCOL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRWORKERPROTOCOL_H

#include <cstdint>

namespace snow_shot::presentation::ocr_worker_protocol {

constexpr std::uint32_t kRequestMagic = 0x534f4352;
constexpr std::uint32_t kResponseMagic = 0x534f4353;
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kStatusSuccess = 0;
constexpr std::uint32_t kStatusFailure = 1;
constexpr std::uint64_t kMaximumImageBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumLineCount = 1'000'000;
constexpr std::uint64_t kMaximumTextBytes = 64ull * 1024ull * 1024ull;

#pragma pack(push, 1)
struct RequestHeader {
    std::uint32_t magic = kRequestMagic;
    std::uint32_t version = kVersion;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t strideBytes = 0;
    std::uint8_t useDirectMl = 0;
    std::uint8_t reserved[3]{};
    std::uint64_t rgbaLength = 0;
};

struct ResponseHeader {
    std::uint32_t magic = kResponseMagic;
    std::uint32_t version = kVersion;
    std::uint32_t status = kStatusFailure;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t strideBytes = 0;
    std::uint64_t rgbaLength = 0;
    std::uint64_t lineCount = 0;
    std::uint64_t errorLength = 0;
};

struct LineHeader {
    std::uint64_t textLength = 0;
    float confidence = 0.0f;
    float points[8]{};
    std::uint8_t foreground[4]{};
};
#pragma pack(pop)

static_assert(sizeof(RequestHeader) == 32);
static_assert(sizeof(ResponseHeader) == 48);
static_assert(sizeof(LineHeader) == 48);

} // namespace snow_shot::presentation::ocr_worker_protocol

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRWORKERPROTOCOL_H
