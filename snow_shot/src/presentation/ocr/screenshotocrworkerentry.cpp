#include "screenshotocrworkerentry.h"

#include "screenshotocrworkerprotocol.h"

#include "snow_ocr_c.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace snow_shot::presentation {
namespace {
namespace protocol = ocr_worker_protocol;

struct OcrEngineDeleter {
    void operator()(SnowOcrEngine* engine) const {
        snow_ocr_engine_destroy(engine);
    }
};

struct OcrResultDeleter {
    void operator()(SnowOcrResult* result) const {
        snow_ocr_result_destroy(result);
    }
};

struct OcrOwnedImageDeleter {
    void operator()(SnowOcrOwnedImage* image) const {
        snow_ocr_owned_image_destroy(image);
    }
};

using OcrEngineHandle = std::unique_ptr<SnowOcrEngine, OcrEngineDeleter>;
using OcrResultHandle = std::unique_ptr<SnowOcrResult, OcrResultDeleter>;
using OcrOwnedImageHandle = std::unique_ptr<SnowOcrOwnedImage, OcrOwnedImageDeleter>;

bool readExact(void* destination, std::size_t length) {
    auto* output = static_cast<std::uint8_t*>(destination);
    std::size_t consumed = 0;
    while (consumed < length) {
        const std::size_t count = std::fread(output + consumed, 1, length - consumed, stdin);
        if (count == 0) {
            return false;
        }
        consumed += count;
    }
    return true;
}

bool writeExact(const void* source, std::size_t length) {
    const auto* input = static_cast<const std::uint8_t*>(source);
    std::size_t written = 0;
    while (written < length) {
        const std::size_t count = std::fwrite(input + written, 1, length - written, stdout);
        if (count == 0) {
            return false;
        }
        written += count;
    }
    return true;
}

std::string lastOcrError() {
    const char* message = snow_ocr_last_error_message();
    if (message == nullptr || *message == '\0') {
        return "Text recognition failed";
    }
    return message;
}

int sendFailure(std::string message) {
    if (message.size() > ocr_worker_protocol::kMaximumTextBytes) {
        message.resize(static_cast<std::size_t>(ocr_worker_protocol::kMaximumTextBytes));
    }
    protocol::ResponseHeader response;
    response.errorLength = message.size();
    if (!writeExact(&response, sizeof(response)) ||
        (!message.empty() && !writeExact(message.data(), message.size())) ||
        std::fflush(stdout) != 0) {
        return 3;
    }
    return 0;
}

bool validRequest(const protocol::RequestHeader& request) {
    if (request.magic != protocol::kRequestMagic || request.version != protocol::kVersion ||
        request.width == 0 || request.height == 0 || request.useDirectMl > 1 ||
        request.reserved[0] != 0 || request.reserved[1] != 0 || request.reserved[2] != 0 ||
        request.rgbaLength == 0 || request.rgbaLength > protocol::kMaximumImageBytes) {
        return false;
    }
    const std::uint64_t rowBytes = static_cast<std::uint64_t>(request.width) * 4;
    return request.strideBytes >= rowBytes &&
           static_cast<std::uint64_t>(request.strideBytes) * request.height ==
               request.rgbaLength;
}

bool validOwnedImageInfo(const SnowOcrImageInfoV1& info, std::uint32_t expectedWidth,
                         std::uint32_t expectedHeight, std::uint64_t* requiredLength) {
    if (info.rgba_bytes == nullptr || info.width != expectedWidth || info.height != expectedHeight) {
        return false;
    }
    const std::uint64_t rowBytes = static_cast<std::uint64_t>(info.width) * 4;
    const std::uint64_t required = static_cast<std::uint64_t>(info.stride_bytes) * info.height;
    if (info.stride_bytes < rowBytes || required == 0 ||
        required > protocol::kMaximumImageBytes || required > info.rgba_len) {
        return false;
    }
    *requiredLength = required;
    return true;
}

OcrEngineHandle createEngine(bool useDirectMl) {
    SnowOcrRuntimeInfoV1 runtimeInfo{static_cast<std::uint32_t>(sizeof(SnowOcrRuntimeInfoV1)), 0};
    const std::uint32_t physicalCores =
        snow_ocr_runtime_info_v1(&runtimeInfo) != 0 ? runtimeInfo.physical_core_count : 1;
    const std::uint32_t threadBudget = (std::max)(1u, physicalCores / 2u);
    const SnowOcrEngineConfigV2 config{
        static_cast<std::uint32_t>(sizeof(SnowOcrEngineConfigV2)),
        threadBudget,
        1u,
        threadBudget,
        1,
        static_cast<std::uint8_t>(useDirectMl ? 1 : 0),
        {0, 0},
    };
    return OcrEngineHandle(snow_ocr_engine_create_with_config_v2(&config));
}
} // namespace

int runScreenshotOcrWorker() {
#if defined(_WIN32)
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1) {
        return 2;
    }
#endif

    protocol::RequestHeader requestHeader;
    if (!readExact(&requestHeader, sizeof(requestHeader))) {
        return 2;
    }
    if (!validRequest(requestHeader) ||
        requestHeader.rgbaLength >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return sendFailure("Invalid OCR worker request");
    }

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(requestHeader.rgbaLength));
    if (!readExact(rgba.data(), rgba.size())) {
        return sendFailure("Incomplete OCR worker request");
    }

    OcrEngineHandle engine = createEngine(requestHeader.useDirectMl != 0);
    if (engine == nullptr) {
        return sendFailure(lastOcrError());
    }
    const SnowOcrRequestV1 request{
        static_cast<std::uint32_t>(sizeof(SnowOcrRequestV1)),
        requestHeader.width,
        requestHeader.height,
        requestHeader.strideBytes,
        rgba.data(),
        rgba.size(),
    };
    OcrResultHandle result(snow_ocr_engine_recognize_rgba(engine.get(), &request));
    if (result == nullptr) {
        return sendFailure(lastOcrError());
    }

    const std::size_t lineCount = snow_ocr_result_line_count(result.get());
    if (lineCount > protocol::kMaximumLineCount) {
        return sendFailure("OCR worker returned too many text lines");
    }
    std::uint64_t totalTextBytes = 0;
    for (std::size_t lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
        SnowOcrLineInfoV1 line{};
        line.struct_size = static_cast<std::uint32_t>(sizeof(SnowOcrLineInfoV1));
        if (snow_ocr_result_line(result.get(), lineIndex, &line) == 0 ||
            (line.text_len > 0 && line.text_utf8 == nullptr) ||
            line.text_len > protocol::kMaximumTextBytes - totalTextBytes) {
            return sendFailure(lastOcrError());
        }
        totalTextBytes += line.text_len;
    }

    OcrOwnedImageHandle image(snow_ocr_result_take_image(result.get()));
    if (image == nullptr) {
        return sendFailure(lastOcrError());
    }
    SnowOcrImageInfoV1 imageInfo{};
    imageInfo.struct_size = static_cast<std::uint32_t>(sizeof(SnowOcrImageInfoV1));
    std::uint64_t imageLength = 0;
    if (snow_ocr_owned_image_info(image.get(), &imageInfo) == 0 ||
        !validOwnedImageInfo(imageInfo, requestHeader.width, requestHeader.height, &imageLength)) {
        return sendFailure(lastOcrError());
    }

    protocol::ResponseHeader response;
    response.status = protocol::kStatusSuccess;
    response.width = imageInfo.width;
    response.height = imageInfo.height;
    response.strideBytes = imageInfo.stride_bytes;
    response.rgbaLength = imageLength;
    response.lineCount = lineCount;
    if (!writeExact(&response, sizeof(response)) ||
        !writeExact(imageInfo.rgba_bytes, static_cast<std::size_t>(imageLength))) {
        return 3;
    }

    for (std::size_t lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
        SnowOcrLineInfoV1 line{};
        line.struct_size = static_cast<std::uint32_t>(sizeof(SnowOcrLineInfoV1));
        if (snow_ocr_result_line(result.get(), lineIndex, &line) == 0) {
            return 3;
        }
        protocol::LineHeader serialized;
        serialized.textLength = line.text_len;
        serialized.confidence = line.confidence;
        std::memcpy(serialized.points, line.quad.points, sizeof(serialized.points));
        serialized.foreground[0] = line.foreground.red;
        serialized.foreground[1] = line.foreground.green;
        serialized.foreground[2] = line.foreground.blue;
        serialized.foreground[3] = line.foreground.alpha;
        if (!writeExact(&serialized, sizeof(serialized)) ||
            (line.text_len > 0 && !writeExact(line.text_utf8, line.text_len))) {
            return 3;
        }
    }
    return std::fflush(stdout) == 0 ? 0 : 3;
}

} // namespace snow_shot::presentation
