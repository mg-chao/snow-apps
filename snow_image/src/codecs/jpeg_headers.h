#pragma once

#include <cstddef>
#include <cstdio>
#include <jconfig.h>

// Qt's bundled JPEG uses int for boolean, whereas the Windows RPC headers
// included by pixel storage use unsigned char. Keep the selected JPEG ABI
// without redefining the Windows type. Other JPEG configurations already
// provide their boolean through HAVE_BOOLEAN and must retain that type.
#if defined(HAVE_BOOLEAN)
#include <jpeglib.h>
#else
#define boolean snow_image_jpeg_boolean
#include <jpeglib.h>
#undef boolean
#endif
