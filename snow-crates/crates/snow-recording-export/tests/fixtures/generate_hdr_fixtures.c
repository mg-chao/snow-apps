// SPDX-License-Identifier: GPL-3.0-or-later
// Regenerate with a Main10-capable FFmpeg/libx265 build; see README.md.
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int status) {
    if (status < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(status, message, sizeof(message));
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void write_packets(AVCodecContext* encoder, AVFormatContext* output, AVStream* stream,
                          AVPacket* packet, int step) {
    int status;
    while ((status = avcodec_receive_packet(encoder, packet)) >= 0) {
        packet->duration = step;
        av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
        packet->stream_index = stream->index;
        check(av_interleaved_write_frame(output, packet));
        av_packet_unref(packet);
    }
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF)
        check(status);
}

static void generate(const char* path, int frames, int step) {
    AVFormatContext* output = NULL;
    check(avformat_alloc_output_context2(&output, NULL, "mp4", path));
    const AVCodec* codec = avcodec_find_encoder_by_name("libx265");
    if (!codec) {
        fprintf(stderr, "libx265 is required to regenerate fixtures\n");
        exit(1);
    }
    AVCodecContext* encoder = avcodec_alloc_context3(codec);
    encoder->width = 32;
    encoder->height = 24;
    encoder->pix_fmt = AV_PIX_FMT_YUV420P10LE;
    encoder->time_base = (AVRational){1, 30};
    encoder->framerate = (AVRational){30, 1};
    encoder->color_range = AVCOL_RANGE_MPEG;
    encoder->colorspace = AVCOL_SPC_BT2020_NCL;
    encoder->color_primaries = AVCOL_PRI_BT2020;
    encoder->color_trc = AVCOL_TRC_SMPTE2084;
    encoder->profile = 2;
    encoder->codec_tag = MKTAG('h', 'v', 'c', '1');
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    encoder->thread_count = 1;
    check(av_opt_set(encoder->priv_data, "preset", "ultrafast", 0));
    check(av_opt_set(
        encoder->priv_data, "x265-params",
        "lossless=1:bframes=0:rc-lookahead=0:frame-threads=1:pools=none:log-level=error", 0));
    check(avcodec_open2(encoder, codec, NULL));
    AVStream* stream = avformat_new_stream(output, NULL);
    stream->time_base = encoder->time_base;
    check(avcodec_parameters_from_context(stream->codecpar, encoder));
    check(avio_open(&output->pb, path, AVIO_FLAG_WRITE));
    check(avformat_write_header(output, NULL));
    AVFrame* frame = av_frame_alloc();
    frame->format = encoder->pix_fmt;
    frame->width = encoder->width;
    frame->height = encoder->height;
    frame->color_range = encoder->color_range;
    frame->colorspace = encoder->colorspace;
    frame->color_primaries = encoder->color_primaries;
    frame->color_trc = encoder->color_trc;
    check(av_frame_get_buffer(frame, 32));
    AVPacket* packet = av_packet_alloc();
    for (int index = 0; index < frames; ++index) {
        check(av_frame_make_writable(frame));
        for (int plane = 0; plane < 3; ++plane) {
            const int width = plane == 0 ? 32 : 16;
            const int height = plane == 0 ? 24 : 12;
            const uint16_t value = plane == 0 ? (uint16_t)(256 * (index + 1)) : 512;
            for (int y = 0; y < height; ++y) {
                uint16_t* row = (uint16_t*)(frame->data[plane] + y * frame->linesize[plane]);
                for (int x = 0; x < width; ++x)
                    row[x] = value;
            }
        }
        frame->pts = index * step;
        check(avcodec_send_frame(encoder, frame));
        write_packets(encoder, output, stream, packet, step);
    }
    check(avcodec_send_frame(encoder, NULL));
    write_packets(encoder, output, stream, packet, step);
    check(av_write_trailer(output));
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
    check(avio_closep(&output->pb));
    avformat_free_context(output);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: generate_hdr_fixtures two-frame.mp4 sparse.mp4\n");
        return 1;
    }
    generate(argv[1], 2, 15);
    generate(argv[2], 3, 30);
    return 0;
}
