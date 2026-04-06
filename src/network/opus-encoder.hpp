#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
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

#include "network/stream-router-utils.hpp"

namespace websocket_server_detail {

struct OpusEnc {
    bool disabled = false;
    bool flush_pending = false;
    int  input_sample_rate = 0;
    int  output_sample_rate = 0;
    int  channels = 0;
    int  frame_size = 0;
    int  bitrate_kbps = 0;
    int  complexity = 10;
    int64_t pts = 0;

    AVCodecContext* ctx = nullptr;
    SwrContext*     swr = nullptr;
    AVAudioFifo*    fifo = nullptr;
    AVFrame*        frame = nullptr;
    AVPacket*       pkt = nullptr;

    void reset() {
        if (pkt)   { av_packet_free(&pkt); }
        if (frame) { av_frame_free(&frame); }
        if (fifo)  { av_audio_fifo_free(fifo); fifo = nullptr; }
        if (swr)   { swr_free(&swr); }
        if (ctx)   { avcodec_free_context(&ctx); }
        input_sample_rate = 0;
        output_sample_rate = 0;
        channels = 0;
        frame_size = 0;
        bitrate_kbps = 0;
        complexity = 10;
        pts = 0;
        disabled = false;
        flush_pending = false;
    }

    bool init(int input_sr, int ch, int bitrate, int target_sr) {
        reset();
        const AVCodec* codec = avcodec_find_encoder_by_name("libopus");
        if (!codec) codec = avcodec_find_encoder_by_name("opus");
        if (!codec) {
            blog(LOG_WARNING, "[obs-delay-stream] Opus encoder not found (libopus/opus)");
            disabled = true;
            return false;
        }

        ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            disabled = true;
            return false;
        }

        int output_sr = is_valid_opus_sample_rate(target_sr)
            ? target_sr
            : 48000;
        if (bitrate < 6) bitrate = 6;
        if (bitrate > 510) bitrate = 510;

        ctx->sample_rate = output_sr;
        av_channel_layout_default(&ctx->ch_layout, ch);
        ctx->bit_rate = static_cast<int64_t>(bitrate) * 1000;
        ctx->time_base = AVRational{1, output_sr};
        ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        (void)av_opt_set_int(ctx, "compression_level", complexity, 0);

        const enum AVSampleFormat* sample_fmts = nullptr;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(61, 13, 100)
        sample_fmts = codec->sample_fmts;
#else
        avcodec_get_supported_config(
            ctx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
            (const void**)&sample_fmts, nullptr);
#endif
        ctx->sample_fmt = AV_SAMPLE_FMT_NONE;
        if (sample_fmts) {
            for (const enum AVSampleFormat* fmt = sample_fmts;
                 *fmt != AV_SAMPLE_FMT_NONE; ++fmt) {
                if (*fmt == AV_SAMPLE_FMT_FLT) {
                    ctx->sample_fmt = *fmt;
                    break;
                }
            }
            if (ctx->sample_fmt == AV_SAMPLE_FMT_NONE) {
                ctx->sample_fmt = sample_fmts[0];
            }
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
        pkt = av_packet_alloc();
        if (!frame || !pkt) {
            disabled = true;
            return false;
        }

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

        if (ctx->sample_fmt != AV_SAMPLE_FMT_FLT || input_sr != output_sr) {
            AVChannelLayout in_layout;
            av_channel_layout_default(&in_layout, ch);
            if (swr_alloc_set_opts2(
                    &swr,
                    &ctx->ch_layout,
                    ctx->sample_fmt,
                    output_sr,
                    &in_layout,
                    AV_SAMPLE_FMT_FLT,
                    input_sr,
                    0,
                    nullptr) < 0 || !swr) {
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

        fifo = av_audio_fifo_alloc(ctx->sample_fmt, ch, frame_size * 8);
        if (!fifo) {
            blog(LOG_WARNING, "[obs-delay-stream] Opus fifo alloc failed");
            disabled = true;
            return false;
        }

        input_sample_rate = input_sr;
        output_sample_rate = output_sr;
        channels = ch;
        bitrate_kbps = bitrate;
        // complexity は宣言時に 10 で初期化済み
        pts = 0;
        return true;
    }

    // FIFO 内の完全フレームをすべてエンコードし out へ追加。
    // 端数サンプルは破棄し、エンコーダ予測状態をリセットする。
    // PTS はそのまま継続する（パケットヘッダの連続性を維持）。
    bool drain(uint32_t magic_opus,
               std::vector<std::shared_ptr<std::string>>& out) {
        if (!ctx || !fifo || !frame || !pkt) return true;

        // 完全フレーム分をエンコード＆送出
        while (av_audio_fifo_size(fifo) >= frame_size) {
            if (av_frame_make_writable(frame) < 0) return false;
            frame->nb_samples = frame_size;
            frame->pts = pts;
            pts += frame_size;
            int rd = av_audio_fifo_read(fifo, (void**)frame->data, frame_size);
            if (rd != frame_size) return false;

            int ret = avcodec_send_frame(ctx, frame);
            if (ret < 0) return false;
            while (ret >= 0) {
                ret = avcodec_receive_packet(ctx, pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) return false;
                uint32_t pkt_frames = (pkt->duration > 0)
                    ? (uint32_t)pkt->duration
                    : (uint32_t)frame_size;
                auto p = std::make_shared<std::string>(16 + pkt->size, '\0');
                uint32_t* hdr = reinterpret_cast<uint32_t*>(&(*p)[0]);
                hdr[0] = magic_opus;
                hdr[1] = (uint32_t)output_sample_rate;
                hdr[2] = (uint32_t)channels;
                hdr[3] = pkt_frames;
                std::memcpy(&(*p)[16], pkt->data, pkt->size);
                out.push_back(std::move(p));
                av_packet_unref(pkt);
            }
        }

        // 端数破棄
        int remain = av_audio_fifo_size(fifo);
        if (remain > 0) av_audio_fifo_drain(fifo, remain);

        // エンコーダ予測状態リセット（PTS は維持）
        avcodec_flush_buffers(ctx);
        return true;
    }

    bool feed_fifo(const float* data, size_t frames) {
        if (!ctx || !fifo || !data || frames == 0) return true;

        if (!swr) {
            int next_size = av_audio_fifo_size(fifo) + static_cast<int>(frames);
            if (av_audio_fifo_realloc(fifo, next_size) < 0) return false;
            const uint8_t* in_data[1] = {
                reinterpret_cast<const uint8_t*>(data)
            };
            int wrote = av_audio_fifo_write(fifo, (void**)in_data, static_cast<int>(frames));
            return wrote == static_cast<int>(frames);
        }

        int out_capacity = static_cast<int>(av_rescale_rnd(
            swr_get_delay(swr, input_sample_rate) + static_cast<int64_t>(frames),
            output_sample_rate,
            input_sample_rate,
            AV_ROUND_UP));
        if (out_capacity <= 0) return true;

        uint8_t** conv_data = nullptr;
        int conv_linesize = 0;
        if (av_samples_alloc_array_and_samples(
                &conv_data,
                &conv_linesize,
                channels,
                out_capacity,
                ctx->sample_fmt,
                0) < 0) {
            return false;
        }

        const uint8_t* in_data[1] = {
            reinterpret_cast<const uint8_t*>(data)
        };
        int converted = swr_convert(
            swr,
            conv_data,
            out_capacity,
            in_data,
            static_cast<int>(frames));
        if (converted < 0) {
            av_freep(&conv_data[0]);
            av_freep(&conv_data);
            return false;
        }

        int next_size = av_audio_fifo_size(fifo) + converted;
        if (av_audio_fifo_realloc(fifo, next_size) < 0) {
            av_freep(&conv_data[0]);
            av_freep(&conv_data);
            return false;
        }
        int wrote = av_audio_fifo_write(fifo, (void**)conv_data, converted);
        av_freep(&conv_data[0]);
        av_freep(&conv_data);
        return wrote == converted;
    }
};

} // namespace websocket_server_detail
