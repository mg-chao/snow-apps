#pragma once

#include "codec_registry.h"

namespace snow::image::internal {

class TiffCodec final : public Codec {
  public:
    [[nodiscard]] Format format() const noexcept override {
        return Format::tiff;
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "LibTIFF";
    }
    [[nodiscard]] CodecCapability capabilities() const noexcept override;
    [[nodiscard]] int probe(std::span<const std::byte> header,
                            std::string_view name_hint) const noexcept override;
    [[nodiscard]] Result<DocumentInfo> inspect(const Input& input, const DecodeOptions& options,
                                               std::stop_token stop) const override;
    [[nodiscard]] Result<Document> decode(const Input& input, const DecodeOptions& options,
                                          std::stop_token stop) const override;
    [[nodiscard]] Result<void> decode_to_sink(const Input& input, PixelSink& sink,
                                              const DecodeOptions& options,
                                              std::stop_token stop) const override;
    [[nodiscard]] Result<EncodedArtifactReceipt>
    encode_to_sink(const Document& document, const Output& output, const EncodeOptions& options,
                   std::stop_token stop) const override;
    [[nodiscard]] Result<EncodedArtifactReceipt>
    encode_raster_to_sink(const RasterSource& source, const Output& output,
                          const EncodeOptions& options, std::stop_token stop) const override;
    [[nodiscard]] RasterEncodeRoute
    raster_encode_route(const DocumentDescriptor& descriptor,
                        const EncodeOptions& options) const noexcept override;
};

} // namespace snow::image::internal
