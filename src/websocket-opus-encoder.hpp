#pragma once

#include <cstdint>
#include <cstring>

#include <vector>

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#ifdef __cplusplus
}
#endif

namespace websocket_server_detail {

struct OpusEnc {
    bool disabled = false;
    int  sample_rate = 0;
    int  channels = 0;
    int  frame_size = 0;
    int64_t pts = 0;

    size_t in_offset_frames = 0;
    std::vector<float> in_buf;

    AVCodecContext* ctx = nullptr;
    SwrContext*     swr = nullptr;
    AVFrame*        frame = nullptr;
    AVPacket*       pkt = nullptr;

    void reset() {
        if (pkt)   { av_packet_free(&pkt); }
        if (frame) { av_frame_free(&frame); }
        if (swr)   { swr_free(&swr); }
        if (ctx)   { avcodec_free_context(&ctx); }
        in_buf.clear();
        in_offset_frames = 0;
        sample_rate = 0;
        channels = 0;
        frame_size = 0;
        pts = 0;
        disabled = false;
    }

    bool init(int sr, int ch, int bitrate_kbps) {
        reset();
        const AVCodec* codec = avcodec_find_encoder_by_name("libopus");
        if (!codec) codec = avcodec_find_encoder_by_name("opus");
        if (!codec) {
            blog(LOG_WARNING, "[obs-delay-stream] Opus encoder not found (libopus/opus)");
            disabled = true;
            return false;
        }

        ctx = avcodec_alloc_context3(codec);
        if (!ctx) { disabled = true; return false; }

        ctx->sample_rate = sr;
        av_channel_layout_default(&ctx->ch_layout, ch);
        ctx->bit_rate = (bitrate_kbps > 0) ? (int64_t)bitrate_kbps * 1000 : 0;
        ctx->time_base = AVRational{1, sr};
        ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

        const enum AVSampleFormat* sample_fmts = nullptr;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(61, 13, 100)
        sample_fmts = codec->sample_fmts;
#else
        avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                     (const void**)&sample_fmts, NULL);
#endif
        ctx->sample_fmt = AV_SAMPLE_FMT_NONE;
        if (sample_fmts) {
            for (const enum AVSampleFormat* fmt = sample_fmts; *fmt != AV_SAMPLE_FMT_NONE; ++fmt) {
                if (*fmt == AV_SAMPLE_FMT_FLT) { ctx->sample_fmt = *fmt; break; }
            }
            if (ctx->sample_fmt == AV_SAMPLE_FMT_NONE)
                ctx->sample_fmt = sample_fmts[0];
        } else {
            ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        }

        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            blog(LOG_WARNING, "[obs-delay-stream] Opus encoder open failed");
            disabled = true;
            return false;
        }

        frame_size = ctx->frame_size;
        if (frame_size <= 0) {
            blog(LOG_WARNING, "[obs-delay-stream] Opus frame_size unavailable");
            disabled = true;
            return false;
        }

        frame = av_frame_alloc();
        pkt   = av_packet_alloc();
        if (!frame || !pkt) { disabled = true; return false; }

        frame->nb_samples = frame_size;
        frame->format = ctx->sample_fmt;
        frame->sample_rate = ctx->sample_rate;
        if (av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout) < 0) {
            disabled = true;
            return false;
        }
        if (av_frame_get_buffer(frame, 0) < 0) {
            blog(LOG_WARNING, "[obs-delay-stream] Opus frame buffer alloc failed");
            disabled = true;
            return false;
        }

        if (ctx->sample_fmt != AV_SAMPLE_FMT_FLT) {
            AVChannelLayout in_layout;
            av_channel_layout_default(&in_layout, ch);
            if (swr_alloc_set_opts2(&swr,
                    &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate,
                    &in_layout, AV_SAMPLE_FMT_FLT, ctx->sample_rate,
                    0, nullptr) < 0 || !swr) {
                blog(LOG_WARNING, "[obs-delay-stream] Opus swr_alloc failed");
                disabled = true;
                return false;
            }
            if (swr_init(swr) < 0) {
                blog(LOG_WARNING, "[obs-delay-stream] Opus swr_init failed");
                disabled = true;
                return false;
            }
        }

        sample_rate = sr;
        channels = ch;
        pts = 0;
        return true;
    }
};

} // namespace websocket_server_detail
