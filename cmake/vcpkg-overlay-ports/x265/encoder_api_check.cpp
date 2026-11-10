#include <x265.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
bool encodeDepth(int depth) {
    const x265_api* api = x265_api_get(depth);
    if (!api || api->bit_depth != depth ||
        api->sizeof_param != static_cast<int>(sizeof(x265_param)) ||
        api->sizeof_picture != static_cast<int>(sizeof(x265_picture))) {
        std::fprintf(stderr, "x265 %d-bit API is unavailable or incompatible\n", depth);
        return false;
    }
    x265_param* parameters = api->param_alloc();
    if (!parameters) {
        return false;
    }
    const bool configured = api->param_default_preset(parameters, "ultrafast", "zerolatency") == 0 &&
                            api->param_parse(parameters, "pools", "none") == 0 &&
                            parameters->internalBitDepth == depth;
    if (!configured) {
        api->param_free(parameters);
        return false;
    }
    parameters->sourceWidth = 64;
    parameters->sourceHeight = 64;
    parameters->fpsNum = 30;
    parameters->fpsDenom = 1;
    parameters->frameNumThreads = 1;
    parameters->logLevel = X265_LOG_NONE;
    parameters->bEmitInfoSEI = 0;
    const bool profile = api->param_apply_profile(parameters, depth == 10 ? "main10" : "main") == 0;
    x265_encoder* encoder = profile ? api->encoder_open(parameters) : nullptr;
    if (!encoder) {
        api->param_free(parameters);
        return false;
    }
    std::array<std::uint8_t, 64 * 64 * 3 / 2> eightBit;
    std::array<std::uint16_t, 64 * 64 * 3 / 2> tenBit;
    eightBit.fill(128);
    tenBit.fill(512);
    x265_picture picture{};
    api->picture_init(parameters, &picture);
    picture.bitDepth = depth;
    const int bytes = depth == 10 ? 2 : 1;
    auto* pixels = depth == 10 ? reinterpret_cast<std::uint8_t*>(tenBit.data()) : eightBit.data();
    picture.planes[0] = pixels;
    picture.planes[1] = pixels + 64 * 64 * bytes;
    picture.planes[2] = pixels + (64 * 64 + 32 * 32) * bytes;
    picture.stride[0] = 64 * bytes;
    picture.stride[1] = 32 * bytes;
    picture.stride[2] = 32 * bytes;
    x265_nal* nals = nullptr;
    std::uint32_t nalCount = 0;
    bool success = api->encoder_headers(encoder, &nals, &nalCount) > 0 && nalCount != 0;
    int frames = 0;
    for (int index = 0; index < 3 && success; ++index) {
        picture.pts = index;
        const int encoded = api->encoder_encode(encoder, &nals, &nalCount, &picture, nullptr);
        success = encoded >= 0;
        frames += encoded > 0 ? 1 : 0;
    }
    while (success) {
        const int encoded = api->encoder_encode(encoder, &nals, &nalCount, nullptr, nullptr);
        success = encoded >= 0;
        if (encoded <= 0) {
            break;
        }
        ++frames;
    }
    api->encoder_close(encoder);
    api->param_free(parameters);
    api->cleanup();
    std::printf("x265 %d-bit public API: %s, encoded frames: %d\n", depth,
                success && frames == 3 ? "pass" : "fail", frames);
    return success && frames == 3;
}

bool unavailableDepth(int depth) {
    int error = X265_API_QUERY_ERR_NONE;
    const bool unavailable = x265_api_get(depth) == nullptr &&
                             x265_api_query(depth, X265_BUILD, &error) == nullptr &&
                             error == X265_API_QUERY_ERR_LIB_NOT_FOUND;
    std::printf("x265 %d-bit get/query unavailable: %s\n", depth, unavailable ? "pass" : "fail");
    return unavailable;
}
} // namespace

int main(int argc, char** argv) {
    const bool combined = argc == 2 && std::strcmp(argv[1], "--combined") == 0;
    if (argc > 2 || (argc == 2 && !combined && std::strcmp(argv[1], "--8bit-only") != 0)) {
        std::fprintf(stderr, "Usage: encoder_api_check [--8bit-only|--combined]\n");
        return 2;
    }
    const bool success = encodeDepth(8) &&
                         (combined ? encodeDepth(10) : unavailableDepth(10) && unavailableDepth(12));
    return success ? 0 : 1;
}
