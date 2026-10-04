#include <snow/memory/pixel_array.h>
#include <rpcndr.h>
#include "../src/codecs/jpeg_headers.h"

#include <type_traits>

#if !defined(_WIN32)
#error This fixture checks the Windows RPC and JPEG include boundary.
#endif

static_assert(std::is_same_v<boolean, unsigned char>);
#if defined(SNOW_TEST_QT_BUNDLED_JPEG)
static_assert(JPEG_LIB_VERSION == 80);
static_assert(std::is_same_v<decltype(jpeg_compress_struct::raw_data_in), int>);
static_assert(
    std::is_same_v<decltype(jpeg_destination_mgr::empty_output_buffer), int (*)(j_compress_ptr)>);
#else
static_assert(JPEG_LIB_VERSION == 62);
static_assert(std::is_same_v<decltype(jpeg_compress_struct::raw_data_in), unsigned char>);
static_assert(std::is_same_v<decltype(jpeg_destination_mgr::empty_output_buffer),
                             unsigned char (*)(j_compress_ptr)>);
#endif

#if defined(boolean)
#error The JPEG boolean macro must not escape the include boundary.
#endif

int main() {
    return 0;
}
