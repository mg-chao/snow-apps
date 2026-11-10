#ifndef SNOW_SHOT_IMAGE_SUPPORTEDIMAGEFILES_H
#define SNOW_SHOT_IMAGE_SUPPORTEDIMAGEFILES_H

#include <QStringList>

namespace snow_shot::image {

enum class ImageFileCodec { Gif, Png, Jpeg, WebP, Jxl, Avif, Bmp, Tiff };

struct ImageFileFormat {
    const char* suffix;
    ImageFileCodec codec;
};

inline constexpr ImageFileFormat kImageFileFormats[] = {
    {"gif", ImageFileCodec::Gif},  {"apng", ImageFileCodec::Png},  {"png", ImageFileCodec::Png},
    {"jpg", ImageFileCodec::Jpeg}, {"jpeg", ImageFileCodec::Jpeg}, {"webp", ImageFileCodec::WebP},
    {"jxl", ImageFileCodec::Jxl},  {"avif", ImageFileCodec::Avif}, {"bmp", ImageFileCodec::Bmp},
    {"tif", ImageFileCodec::Tiff}, {"tiff", ImageFileCodec::Tiff},
};

[[nodiscard]] inline QStringList supportedImageFileExtensions() {
    QStringList extensions;
    for (const auto& format : kImageFileFormats)
        extensions.append(QString::fromLatin1(format.suffix));
    return extensions;
}

} // namespace snow_shot::image

#endif // SNOW_SHOT_IMAGE_SUPPORTEDIMAGEFILES_H
