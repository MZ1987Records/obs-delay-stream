#pragma once

/*
 * websocket-server.hpp
 *
 * StreamRouter: 単一ポート(19000)でパスルーティングを行う WebSocketサーバー
 *
 * 接続URL: ws://[IP]:19000/[配信ID]/[ch番号(1-10)]
 *   例: ws://192.168.1.10:19000/myshow2024/1
 *
 * 配信IDが一致するchへのみ音声・pingを届ける。
 * 配信IDが異なる接続は別セッションとして完全に分離される。
 *
 * 【音声バイナリフォーマット】
 *   PCM16: [4B magic=0x41554449][4B SR][4B CH][4B frames][int16*frames*CH]
 *   OPUS : [4B magic=0x4F505553][4B SR][4B CH][4B frames][Opus packet bytes]
 *
 * 【制御メッセージ (テキストJSON)】
 *   OBS → Browser: {"type":"ping","seq":N,"t":T}
 *   Browser → OBS: {"type":"pong","seq":N}
 *   OBS → Browser: {"type":"latency_result","avg_rtt":X,"one_way":Y,"min":A,"max":B,"samples":N}
 *   OBS → Browser: {"type":"apply_delay","ms":X}
 *   OBS → Browser: {"type":"session_info","stream_id":"xxx","ch":N,"memo":"..."}  ← 接続直後に送信
 *   OBS → Browser: {"type":"memo","ch":N,"memo":"..."}  ← メモ変更通知
 *   Browser → OBS: {"type":"audio_codec","mode":"pcm"}  ← Opus不可時のPCM要求
 *
 * v2.0 changes:
 *   - send_audio() を非ブロッキング化 (ASIO::post 経由で送信)
 *   - .detach() を廃止、全計測スレッドを join() 管理
 *   - stop() の正しい停止順序
 */

#define ASIO_STANDALONE
#define _WEBSOCKETPP_CPP11_STL_

// Prevent SIMDe from redefining standard integer types
// (SIMDe is pulled in by obs-module.h and can conflict with websocketpp)
#ifndef SIMDE_NO_NATIVE
#define SIMDE_NO_NATIVE
#endif

// Windows headers before websocketpp/asio
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// Standard types before websocketpp
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <thread>
#include <mutex>
#include <map>
#include <set>
#include <array>
#include <vector>
#include <chrono>
#include <string>
#include <functional>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <memory>

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

using WsServer   = websocketpp::server<websocketpp::config::asio>;
using ConnHandle = websocketpp::connection_hdl;
using Clock      = std::chrono::steady_clock;
using Ms         = std::chrono::duration<double, std::milli>;

static constexpr uint32_t MAGIC_AUDI = 0x41554449u; // "AUDI"
static constexpr uint32_t MAGIC_OPUS = 0x4F505553u; // "OPUS"
static constexpr int MAX_CH = 10;

// ============================================================
// LatencyResult
// ============================================================
struct LatencyResult {
    bool   valid       = false;
    double avg_rtt_ms  = 0.0;
    double avg_one_way = 0.0;
    double min_rtt_ms  = 0.0;
    double max_rtt_ms  = 0.0;
    int    samples     = 0;
};

// ============================================================
// ConnInfo: 各接続のメタ情報
// ============================================================
struct ConnInfo {
    std::string stream_id; // 配信ID
    int         ch = -1;   // 0-indexed ch番号 (0〜9)
    bool        force_pcm = false; // クライアント要求でPCM固定
};

// ============================================================
// ChannelState: ch毎の状態 (配信ID + ch番号でキー)
// ============================================================
struct ChannelState {
    // 接続ハンドル集合
    std::set<ConnHandle, std::owner_less<ConnHandle>> conns;

    // RTT計測
    std::map<int, Clock::time_point> ping_times;
    std::vector<double>              rtt_samples;
    std::atomic<bool>                measuring{false};
    LatencyResult                    last_result;

    // 最後に適用された遅延 (ms)。負値=未設定
    double                           last_applied_delay{-1.0};

    // コールバック (計測完了時)
    std::function<void(const std::string& stream_id, int ch, LatencyResult)> on_result;
};

// ============================================================
// StreamRouter
//   全chを1ポートで管理。配信ID + ch番号でルーティング。
// ============================================================
class StreamRouter {
public:
    StreamRouter()  = default;
    ~StreamRouter() { stop(); }

    // 接続数の変化通知 (sid, ch_0idx, count)
    std::function<void(const std::string&, int, size_t)> on_conn_change;
    // 計測結果通知（sid, ch_0idx, result）
    std::function<void(const std::string&, int, LatencyResult)> on_any_latency_result;

    // ----- 起動 / 停止 -----
    bool start(uint16_t port) {
        if (running_) return true;
        port_ = port;
        auto srv = std::make_shared<WsServer>();
        try {
            srv->set_error_channels(websocketpp::log::elevel::none);
            srv->set_access_channels(websocketpp::log::alevel::none);
            srv->init_asio();
            srv->set_reuse_addr(true);
            srv->set_open_handler([this](ConnHandle h) { on_open(h); });
            srv->set_close_handler([this](ConnHandle h) { on_close(h); });
            srv->set_message_handler([this](ConnHandle h, WsServer::message_ptr m) {
                on_message(h, m);
            });
            srv->set_http_handler([this](ConnHandle h) { on_http(h); });
            srv->listen(port_);
            srv->start_accept();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                server_ptr_ = srv;
            }
            thread_ = std::thread([srv]() { srv->run(); });
            running_ = true;
            return true;
        } catch (...) {
            std::lock_guard<std::mutex> lk(mtx_);
            server_ptr_.reset();
            return false;
        }
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        // 1. 全計測スレッドの停止を要求
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& kv : ch_map_)
                kv.second.measuring = false;
        }

        // 2. 全計測スレッドを join
        join_all_measure_threads();

        // 3. ASIO サーバーを停止
        std::shared_ptr<WsServer> srv;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            srv = server_ptr_;
        }
        if (srv) {
            try { srv->stop_listening(); } catch (...) {}
            try { srv->stop(); } catch (...) {}
        }
        if (thread_.joinable()) thread_.join();

        // 4. 計測結果・適用遅延をキャッシュに退避してから状態をクリア
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& [key, cs] : ch_map_) {
                if (cs.last_result.valid || cs.last_applied_delay >= 0.0)
                    ch_cache_[key] = { cs.last_result, cs.last_applied_delay };
            }
            conn_map_.clear();
            ch_map_.clear();
            reset_opus_state();
        }

        // 5. サーバーオブジェクト破棄
        std::lock_guard<std::mutex> lk(mtx_);
        server_ptr_.reset();
    }

    // ----- 配信ID設定 -----
    void set_stream_id(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        stream_id_ = sanitize_id(id);
    }
    std::string stream_id() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return stream_id_;
    }

    void set_sub_memo(int ch, const std::string& memo) {
        if (ch < 0 || ch >= MAX_CH) return;
        std::string sid;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (sub_memo_[ch] == memo) return;
            sub_memo_[ch] = memo;
            sid = stream_id_;
        }
        if (sid.empty()) return;
        std::string msg = "{\"type\":\"memo\",\"ch\":" + std::to_string(ch + 1)
                        + ",\"memo\":\"" + json_escape(memo) + "\"}";
        broadcast_text(sid, ch, msg);
    }

    // ----- 音声送信 (ch: 0-indexed) -----
    // 音声スレッドから呼ばれる。非ブロッキング: ASIO スレッドに委譲。
    void send_audio(int ch, const float* data, size_t frames,
                    uint32_t sample_rate, uint32_t channels)
    {
        if (!running_) return;
        if (opus_reset_pending_.exchange(false, std::memory_order_acq_rel)) {
            reset_opus_state();
        }
        // 送信先ハンドルのスナップショットを取得（短時間ロック）
        std::vector<ConnHandle> targets_opus;
        std::vector<ConnHandle> targets_pcm;
        std::shared_ptr<WsServer> srv;
        const int default_codec = audio_codec_.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto* cs = find_ch(stream_id_, ch);
            if (!cs || cs->conns.empty()) return;
            for (auto& hdl : cs->conns) {
                auto it = conn_map_.find(hdl);
                if (it == conn_map_.end()) continue;
                if (!it->second.force_pcm && default_codec == 0) {
                    targets_opus.push_back(hdl);
                } else {
                    targets_pcm.push_back(hdl);
                }
            }
            srv = server_ptr_;
        }
        if (!srv) return;
        if (targets_opus.empty() && targets_pcm.empty()) return;

        // --- Opus 優先、失敗時 PCM16（Opus対象のみ） ---
        if (!targets_opus.empty() && default_codec == 0) {
            std::vector<std::shared_ptr<std::string>> pkts;
            if (encode_opus_packets(ch, data, frames, sample_rate, channels, pkts)) {
                if (!pkts.empty()) {
                    try {
                        srv->get_io_service().post([srv, targets = std::move(targets_opus),
                                                    pkts = std::move(pkts)]() mutable {
                            for (auto& pkt : pkts) {
                                for (auto& hdl : targets) {
                                    try { srv->send(hdl, *pkt, websocketpp::frame::opcode::binary); }
                                    catch (...) {}
                                }
                            }
                        });
                    } catch (...) {}
                }
                if (targets_pcm.empty()) return;
            } else {
                // Opus失敗時は対象をPCMへ回す
                targets_pcm.insert(targets_pcm.end(), targets_opus.begin(), targets_opus.end());
            }
        }

        if (targets_pcm.empty()) return;

        // パケットを構築（PCM16）
        size_t samples = frames * channels;
        size_t pcm_bytes = samples * sizeof(int16_t);
        auto pkt = std::make_shared<std::string>(16 + pcm_bytes, '\0');
        uint32_t* hdr = reinterpret_cast<uint32_t*>(&(*pkt)[0]);
        hdr[0] = MAGIC_AUDI;
        hdr[1] = sample_rate;
        hdr[2] = channels;
        hdr[3] = static_cast<uint32_t>(frames);
        int16_t* dst = reinterpret_cast<int16_t*>(&(*pkt)[16]);
        for (size_t i = 0; i < samples; ++i) {
            float v = data[i];
            if (!std::isfinite(v)) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            dst[i] = static_cast<int16_t>(std::lrintf(v * 32767.0f));
        }

        // ASIO スレッドに送信を委譲（音声スレッドはブロックしない）
        try {
            srv->get_io_service().post([srv, targets = std::move(targets_pcm),
                                        pkt = std::move(pkt)]() {
                for (auto& hdl : targets) {
                    try { srv->send(hdl, *pkt, websocketpp::frame::opcode::binary); }
                    catch (...) {}
                }
            });
        } catch (...) {}
    }

    // ----- RTT計測 (ch: 0-indexed) -----
    bool start_measurement(int ch, int num_pings = 10, int interval_ms = 150) {
        std::string sid;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto* cs = find_ch(stream_id_, ch);
            if (!cs || cs->conns.empty() || cs->measuring) return false;

            cs->measuring = true;
            cs->ping_times.clear();
            cs->rtt_samples.clear();
            sid = stream_id_;
        }

        // 管理されたスレッドで計測
        auto mt = std::make_unique<MeasureThread>();
        MeasureThread* mt_ptr = mt.get();
        mt->th = std::thread([this, sid, ch, num_pings, interval_ms, mt_ptr]() {
            measure_loop(sid, ch, num_pings, interval_ms);
            mt_ptr->done.store(true, std::memory_order_release);
        });
        {
            std::lock_guard<std::mutex> lk(measure_threads_mtx_);
            measure_threads_.push_back(std::move(mt));
        }
        cleanup_done_measure_threads();
        return true;
    }

    bool is_measuring(int ch) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto* cs = find_ch(stream_id_, ch);
        return cs ? cs->measuring.load() : false;
    }

    LatencyResult last_result(int ch) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto* cs = find_ch(stream_id_, ch);
        return cs ? cs->last_result : LatencyResult{};
    }

    // 計測完了コールバック設定 (ch: 0-indexed)
    void set_on_latency_result(int ch,
        std::function<void(const std::string&, int, LatencyResult)> cb)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& cs = ch_map_[make_key(stream_id_, ch)];
        cs.on_result = std::move(cb);
    }

    void set_http_index_html(std::string html) {
        std::lock_guard<std::mutex> lk(mtx_);
        http_index_html_ = std::move(html);
    }

    // 全コールバックをクリア（破棄前に呼ぶ）
    void clear_callbacks() {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& kv : ch_map_)
            kv.second.on_result = nullptr;
        on_conn_change = nullptr;
        on_any_latency_result = nullptr;
    }

    // 遅延反映通知
    void notify_apply_delay(int ch, double ms) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto& cs = ch_map_[make_key(stream_id_, ch)];
            cs.last_applied_delay = ms;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"type\":\"apply_delay\",\"ms\":%.1f}", ms);
        broadcast_text(stream_id_, ch, buf);
    }

    // 接続数取得 (ch: 0-indexed)
    size_t client_count(int ch) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto* cs = find_ch(stream_id_, ch);
        return cs ? cs->conns.size() : 0;
    }

    // 受信URLを生成
    std::string make_url(const std::string& host, int ch_1indexed) const {
        std::lock_guard<std::mutex> lk(mtx_);
        char buf[256];
        snprintf(buf, sizeof(buf), "ws://%s:%d/%s/%d",
                 host.c_str(), (int)port_,
                 stream_id_.empty() ? "(配信ID未設定)" : stream_id_.c_str(),
                 ch_1indexed);
        return buf;
    }

    uint16_t port() const { return port_; }
    bool     is_running() const { return running_; }

    // ----- 音声コーデック設定 -----
    // 0: Opus, 1: PCM16
    void set_audio_codec(int mode) {
        if (mode != 0 && mode != 1) mode = 0;
        audio_codec_.store(mode, std::memory_order_relaxed);
        opus_reset_pending_.store(true, std::memory_order_release);
    }

    void set_http_root_dir(std::string dir) {
        std::lock_guard<std::mutex> lk(mtx_);
        http_root_dir_ = std::move(dir);
    }

private:
    // ----- Opus Encoder (per channel) -----
    struct OpusEnc {
        bool disabled = false; // 初期化失敗で無効化
        int  sample_rate = 0;
        int  channels = 0;
        int  frame_size = 0;   // samples per channel
        int64_t pts = 0;

        size_t in_offset_frames = 0;
        std::vector<float> in_buf; // interleaved float

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

            // prefer packed float if available
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
                // Opus は通常 frame_size が設定される。未設定なら無効化。
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

    // ----- キー生成 -----
    static std::string make_key(const std::string& sid, int ch) {
        return sid + "/" + std::to_string(ch);
    }

    // ----- 配信IDのサニタイズ (半角英数字のみ許可) -----
    static std::string sanitize_id(const std::string& raw) {
        std::string out;
        for (char c : raw) {
            if (std::isalnum((unsigned char)c))
                out += (char)std::tolower((unsigned char)c);
        }
        return out;
    }

    static std::string json_escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (unsigned char c : s) {
            switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
            }
        }
        return out;
    }

    static std::string url_decode(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = (unsigned char)s[i];
            if (c == '%' && i + 2 < s.size()
                && std::isxdigit((unsigned char)s[i + 1])
                && std::isxdigit((unsigned char)s[i + 2])) {
                auto hex = [](unsigned char v) -> int {
                    if (v >= '0' && v <= '9') return v - '0';
                    if (v >= 'a' && v <= 'f') return 10 + (v - 'a');
                    if (v >= 'A' && v <= 'F') return 10 + (v - 'A');
                    return 0;
                };
                int hi = hex((unsigned char)s[i + 1]);
                int lo = hex((unsigned char)s[i + 2]);
                out.push_back((char)((hi << 4) | lo));
                i += 2;
            } else if (c == '+') {
                out.push_back(' ');
            } else {
                out.push_back((char)c);
            }
        }
        return out;
    }

    // ----- WebSocketパスから (stream_id, ch) を解析 -----
    static bool parse_path(const std::string& path,
                            std::string& stream_id, int& ch_0idx)
    {
        std::string p = path;
        if (!p.empty() && p[0] == '/') p = p.substr(1);

        auto slash = p.find('/');
        if (slash == std::string::npos || slash == 0) return false;

        stream_id = sanitize_id(p.substr(0, slash));
        std::string ch_str = p.substr(slash + 1);
        auto q = ch_str.find_first_of("?#");
        if (q != std::string::npos) ch_str = ch_str.substr(0, q);

        int ch_1idx = std::atoi(ch_str.c_str());
        if (ch_1idx < 1 || ch_1idx > 10) return false;
        ch_0idx = ch_1idx - 1;
        return !stream_id.empty();
    }

    static bool is_safe_rel_path(const std::string& rel) {
        if (rel.empty()) return false;
        if (rel.find("..") != std::string::npos) return false;
        if (rel.find('\\') != std::string::npos) return false;
        if (rel.find(':') != std::string::npos) return false; // Windows drive
        return true;
    }

    static std::string join_path(const std::string& base, const std::string& rel) {
        if (base.empty()) return rel;
        std::string out = base;
        char last = out.back();
        if (last != '/' && last != '\\') out += '/';
        out += rel;
        return out;
    }

    static bool read_file_to_string(const std::string& path, std::string& out) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return false;
        out.assign(std::istreambuf_iterator<char>(ifs),
                   std::istreambuf_iterator<char>());
        return true;
    }

    static const char* guess_content_type(const std::string& path) {
        auto dot = path.find_last_of('.');
        std::string ext = (dot == std::string::npos) ? "" : path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){
            return (char)std::tolower(c);
        });
        if (ext == ".html") return "text/html; charset=utf-8";
        if (ext == ".js" || ext == ".mjs") return "application/javascript; charset=utf-8";
        if (ext == ".css") return "text/css; charset=utf-8";
        if (ext == ".wasm") return "application/wasm";
        if (ext == ".json" || ext == ".map") return "application/json; charset=utf-8";
        if (ext == ".svg") return "image/svg+xml";
        if (ext == ".png") return "image/png";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".gif") return "image/gif";
        return "application/octet-stream";
    }

    // ----- ch_map_検索 -----
    ChannelState* find_ch(const std::string& sid, int ch) {
        auto it = ch_map_.find(make_key(sid, ch));
        return it != ch_map_.end() ? &it->second : nullptr;
    }
    const ChannelState* find_ch(const std::string& sid, int ch) const {
        auto it = ch_map_.find(make_key(sid, ch));
        return it != ch_map_.end() ? &it->second : nullptr;
    }

    // ----- 全計測スレッドを join -----
    void join_all_measure_threads() {
        std::vector<std::unique_ptr<MeasureThread>> threads;
        {
            std::lock_guard<std::mutex> lk(measure_threads_mtx_);
            threads.swap(measure_threads_);
        }
        for (auto& t : threads) {
            if (t && t->th.joinable()) t->th.join();
        }
    }

    void cleanup_done_measure_threads() {
        std::vector<std::unique_ptr<MeasureThread>> done;
        {
            std::lock_guard<std::mutex> lk(measure_threads_mtx_);
            for (auto it = measure_threads_.begin(); it != measure_threads_.end(); ) {
                if ((*it)->done.load(std::memory_order_acquire)) {
                    done.push_back(std::move(*it));
                    it = measure_threads_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& t : done) {
            if (t && t->th.joinable()) t->th.join();
        }
    }

    bool ensure_opus_encoder(int ch, uint32_t sample_rate, uint32_t channels) {
        if (ch < 0 || channels == 0 || sample_rate == 0) return false;
        if ((size_t)(ch + 1) > opus_.size()) opus_.resize(ch + 1);
        OpusEnc& enc = opus_[ch];
        if (enc.disabled) return false;
        if (enc.ctx &&
            enc.sample_rate == (int)sample_rate &&
            enc.channels == (int)channels) return true;
        return enc.init((int)sample_rate, (int)channels,
                        opus_bitrate_kbps_.load(std::memory_order_relaxed));
    }

    void reset_opus_state() {
        for (auto& enc : opus_) enc.reset();
        opus_.clear();
    }

    bool encode_opus_packets(int ch, const float* data, size_t frames,
                             uint32_t sample_rate, uint32_t channels,
                             std::vector<std::shared_ptr<std::string>>& out)
    {
        if (!ensure_opus_encoder(ch, sample_rate, channels)) return false;
        OpusEnc& enc = opus_[ch];
        if (!enc.ctx || enc.disabled) return false;

        const size_t samples = frames * channels;
        enc.in_buf.insert(enc.in_buf.end(), data, data + samples);

        size_t total_frames = (enc.in_buf.size() / channels);
        if (total_frames <= enc.in_offset_frames) return true;
        size_t avail_frames = total_frames - enc.in_offset_frames;

        while (avail_frames >= (size_t)enc.frame_size) {
            const float* in = enc.in_buf.data() + enc.in_offset_frames * channels;
            if (av_frame_make_writable(enc.frame) < 0) {
                enc.disabled = true;
                return false;
            }
            enc.frame->nb_samples = enc.frame_size;
            enc.frame->pts = enc.pts;
            enc.pts += enc.frame_size;

            if (enc.ctx->sample_fmt == AV_SAMPLE_FMT_FLT) {
                std::memcpy(enc.frame->data[0], in,
                            (size_t)enc.frame_size * channels * sizeof(float));
            } else {
                const uint8_t* in_data[1] = {
                    reinterpret_cast<const uint8_t*>(in)
                };
                int conv = swr_convert(enc.swr, enc.frame->data, enc.frame_size,
                                       in_data, enc.frame_size);
                if (conv < 0) {
                    enc.disabled = true;
                    return false;
                }
            }

            int ret = avcodec_send_frame(enc.ctx, enc.frame);
            if (ret < 0) {
                enc.disabled = true;
                return false;
            }

            while (ret >= 0) {
                ret = avcodec_receive_packet(enc.ctx, enc.pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    enc.disabled = true;
                    return false;
                }
                uint32_t pkt_frames = (enc.pkt->duration > 0)
                    ? (uint32_t)enc.pkt->duration
                    : (uint32_t)enc.frame_size;
                auto pkt = std::make_shared<std::string>(16 + enc.pkt->size, '\0');
                uint32_t* hdr = reinterpret_cast<uint32_t*>(&(*pkt)[0]);
                hdr[0] = MAGIC_OPUS;
                hdr[1] = sample_rate;
                hdr[2] = channels;
                hdr[3] = pkt_frames;
                std::memcpy(&(*pkt)[16], enc.pkt->data, enc.pkt->size);
                out.push_back(std::move(pkt));
                av_packet_unref(enc.pkt);
            }

            enc.in_offset_frames += enc.frame_size;
            avail_frames -= enc.frame_size;
        }

        // バッファ消費分を詰める
        if (enc.in_offset_frames > 0) {
            size_t consumed_samples = enc.in_offset_frames * channels;
            if (consumed_samples >= enc.in_buf.size()) {
                enc.in_buf.clear();
                enc.in_offset_frames = 0;
            } else if (enc.in_offset_frames >= (size_t)enc.frame_size * 4) {
                enc.in_buf.erase(enc.in_buf.begin(),
                                 enc.in_buf.begin() + consumed_samples);
                enc.in_offset_frames = 0;
            }
        }
        return true;
    }

    // ----- イベントハンドラ -----
    void on_open(ConnHandle h) {
        std::shared_ptr<WsServer> srv;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            srv = server_ptr_;
        }
        if (!srv) return;
        auto con = srv->get_con_from_hdl(h);
        std::string path = con->get_resource();
        std::string sid; int ch;
        if (!parse_path(path, sid, ch)) {
            con->close(websocketpp::close::status::policy_violation,
                       "invalid path: use /stream_id/ch");
            return;
        }
        // 配信IDが設定済みなら一致を要求
        std::string current_sid;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            current_sid = stream_id_;
        }
        if (!current_sid.empty() && sid != current_sid) {
            con->close(websocketpp::close::status::policy_violation,
                       "stream_id_mismatch");
            return;
        }

        std::function<void(const std::string&, int, size_t)> cb;
        size_t count = 0;
        LatencyResult cached_result;
        double cached_delay = -1.0;
        std::string memo;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            conn_map_[h] = { sid, ch };
            auto key = make_key(sid, ch);
            auto& cs = ch_map_[key];
            // 新規エントリかつキャッシュがあれば復元
            if (!cs.last_result.valid && cs.last_applied_delay < 0.0) {
                auto it = ch_cache_.find(key);
                if (it != ch_cache_.end()) {
                    cs.last_result        = it->second.last_result;
                    cs.last_applied_delay = it->second.last_applied_delay;
                }
            }
            cs.conns.insert(h);
            count = cs.conns.size();
            cb = on_conn_change;
            cached_result = cs.last_result;
            cached_delay  = cs.last_applied_delay;
            if (ch >= 0 && ch < MAX_CH) memo = sub_memo_[ch];
        }
        if (cb) cb(sid, ch, count);

        // session_info
        std::string info = "{\"type\":\"session_info\",\"stream_id\":\"" + sid
                         + "\",\"ch\":" + std::to_string(ch + 1);
        if (!memo.empty()) info += ",\"memo\":\"" + json_escape(memo) + "\"";
        info += "}";
        try { srv->send(h, info, websocketpp::frame::opcode::text); }
        catch (...) {}

        // 計測済みの遅延情報があれば再接続クライアントへ即送信
        if (cached_result.valid) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "{\"type\":\"latency_result\","
                "\"avg_rtt\":%.1f,\"one_way\":%.1f,"
                "\"min\":%.1f,\"max\":%.1f,\"samples\":%d}",
                cached_result.avg_rtt_ms, cached_result.avg_one_way,
                cached_result.min_rtt_ms, cached_result.max_rtt_ms,
                cached_result.samples);
            try { srv->send(h, std::string(buf), websocketpp::frame::opcode::text); }
            catch (...) {}
        }
        if (cached_delay >= 0.0) {
            char buf[64];
            snprintf(buf, sizeof(buf),
                "{\"type\":\"apply_delay\",\"ms\":%.1f}", cached_delay);
            try { srv->send(h, std::string(buf), websocketpp::frame::opcode::text); }
            catch (...) {}
        }
    }

    void on_close(ConnHandle h) {
        std::string sid;
        int ch = -1;
        size_t count = 0;
        std::function<void(const std::string&, int, size_t)> cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = conn_map_.find(h);
            if (it == conn_map_.end()) return;
            sid = it->second.stream_id;
            ch = it->second.ch;
            auto* cs = find_ch(sid, ch);
            if (cs) {
                cs->conns.erase(h);
                if (cs->conns.empty()) cs->measuring = false;
                count = cs->conns.size();
            }
            conn_map_.erase(it);
            cb = on_conn_change;
        }
        if (cb) cb(sid, ch, count);
    }

    void on_message(ConnHandle h, WsServer::message_ptr msg) {
        if (msg->get_opcode() != websocketpp::frame::opcode::text) return;
        const std::string& p = msg->get_payload();
        if (p.find("\"pong\"") != std::string::npos) {
            int seq = -1;
            auto pos = p.find("\"seq\":");
            if (pos != std::string::npos) seq = std::atoi(p.c_str() + pos + 6);
            if (seq < 0) return;

            auto now = Clock::now();
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = conn_map_.find(h);
            if (it == conn_map_.end()) return;
            auto* cs = find_ch(it->second.stream_id, it->second.ch);
            if (!cs) return;
            auto pt = cs->ping_times.find(seq);
            if (pt != cs->ping_times.end()) {
                cs->rtt_samples.push_back(Ms(now - pt->second).count());
                cs->ping_times.erase(pt);
            }
            return;
        }

        if (p.find("\"audio_codec\"") != std::string::npos) {
            int mode = -1;
            if (p.find("\"mode\":\"pcm\"") != std::string::npos) mode = 1;
            else if (p.find("\"mode\":\"opus\"") != std::string::npos) mode = 0;
            if (mode < 0) return;

            std::lock_guard<std::mutex> lk(mtx_);
            auto it = conn_map_.find(h);
            if (it == conn_map_.end()) return;
            it->second.force_pcm = (mode == 1);
            return;
        }
    }

    void on_http(ConnHandle h) {
        std::shared_ptr<WsServer> srv;
        std::string body;
        std::string root;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            srv = server_ptr_;
            body = http_index_html_;
            root = http_root_dir_;
        }
        if (!srv) return;
        websocketpp::connection_hdl hdl = h;
        try {
            auto con = srv->get_con_from_hdl(hdl);
            std::string raw = con->get_resource();
            std::string path = raw;
            std::string query;
            auto qpos = path.find('?');
            if (qpos != std::string::npos) {
                query = path.substr(qpos + 1);
                path = path.substr(0, qpos);
            }
            auto hpos = path.find('#');
            if (hpos != std::string::npos) path = path.substr(0, hpos);
            if (path.empty()) path = "/";
            if (path == "/memo") {
                std::string sid_param;
                int ch_1idx = -1;
                size_t pos = 0;
                while (pos < query.size()) {
                    size_t amp = query.find('&', pos);
                    if (amp == std::string::npos) amp = query.size();
                    std::string kv = query.substr(pos, amp - pos);
                    size_t eq = kv.find('=');
                    std::string key = (eq == std::string::npos) ? kv : kv.substr(0, eq);
                    std::string val = (eq == std::string::npos) ? "" : kv.substr(eq + 1);
                    if (key == "sid") sid_param = url_decode(val);
                    else if (key == "ch") ch_1idx = std::atoi(val.c_str());
                    pos = amp + 1;
                }

                std::string sid = sanitize_id(sid_param);
                if (sid.empty() || ch_1idx < 1 || ch_1idx > MAX_CH) {
                    con->set_status(websocketpp::http::status_code::bad_request);
                    con->replace_header("Content-Type", "text/plain; charset=utf-8");
                    con->set_body("Bad Request");
                    return;
                }

                std::string current_sid;
                std::string memo;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    current_sid = stream_id_;
                    if (ch_1idx - 1 >= 0 && ch_1idx - 1 < MAX_CH) {
                        memo = sub_memo_[ch_1idx - 1];
                    }
                }
                if (current_sid.empty() || sid != current_sid) {
                    con->set_status(websocketpp::http::status_code::not_found);
                    con->replace_header("Content-Type", "text/plain; charset=utf-8");
                    con->set_body("Not Found");
                    return;
                }

                std::string body = "{\"ok\":true,\"stream_id\":\"" + current_sid
                                 + "\",\"ch\":" + std::to_string(ch_1idx)
                                 + ",\"memo\":\"" + json_escape(memo) + "\"}";
                con->set_status(websocketpp::http::status_code::ok);
                con->replace_header("Content-Type", "application/json; charset=utf-8");
                con->replace_header("Cache-Control", "no-store");
                con->set_body(std::move(body));
                return;
            }
            if (path == "/" || path == "/index.html") {
                if (!body.empty()) {
                    con->set_status(websocketpp::http::status_code::ok);
                    con->replace_header("Content-Type", "text/html; charset=utf-8");
                    con->replace_header("Cache-Control", "no-store");
                    con->set_body(std::move(body));
                    return;
                }
                if (!root.empty()) {
                    std::string full = join_path(root, "index.html");
                    std::string file;
                    if (read_file_to_string(full, file)) {
                        con->set_status(websocketpp::http::status_code::ok);
                        con->replace_header("Content-Type", "text/html; charset=utf-8");
                        con->replace_header("Cache-Control", "no-store");
                        con->set_body(std::move(file));
                        return;
                    }
                }
            }

            if (!root.empty()) {
                std::string rel = path;
                if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
                if (is_safe_rel_path(rel)) {
                    std::string full = join_path(root, rel);
                    std::string file;
                    if (read_file_to_string(full, file)) {
                        con->set_status(websocketpp::http::status_code::ok);
                        con->replace_header("Content-Type", guess_content_type(full));
                        con->replace_header("Cache-Control", "no-store");
                        con->set_body(std::move(file));
                        return;
                    }
                }
            }

            con->set_status(websocketpp::http::status_code::not_found);
            con->replace_header("Content-Type", "text/plain; charset=utf-8");
            con->set_body("Not Found");
        } catch (...) {
        }
    }

    // ----- 計測ループ -----
    void measure_loop(const std::string& sid, int ch,
                      int num_pings, int interval_ms)
    {
        blog(LOG_INFO, "[obs-delay-stream] latency measure start: sid=%s ch=%d pings=%d",
             sid.c_str(), ch + 1, num_pings);
        for (int seq = 0; seq < num_pings; ++seq) {
            if (!running_) break;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto* cs = find_ch(sid, ch);
                if (!cs || !cs->measuring) break;
                auto srv = server_ptr_;
                if (!srv) break;
                auto t_now = Ms(Clock::now().time_since_epoch()).count();
                cs->ping_times[seq] = Clock::now();
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"ping\",\"seq\":%d,\"t\":%.3f}", seq, t_now);
                for (auto& hdl : cs->conns) {
                    try { srv->send(hdl, std::string(buf),
                                       websocketpp::frame::opcode::text); }
                    catch (...) {}
                }
            }
            // 中断可能なスリープ
            for (int t = 0; t < interval_ms && running_; ++t)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // 中断可能な待機（pong受信を待つ）
        for (int t = 0; t < 600 && running_; ++t)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        finalize_result(sid, ch);
    }

    void finalize_result(const std::string& sid, int ch) {
        LatencyResult r;
        std::function<void(const std::string&, int, LatencyResult)> cb;
        std::function<void(const std::string&, int, LatencyResult)> cb_any;
        bool no_samples = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto* cs = find_ch(sid, ch);
            if (!cs) return;
            cs->measuring = false;

            auto& s = cs->rtt_samples;
            if (!s.empty()) {
                r.valid   = true;
                r.samples = (int)s.size();
                r.min_rtt_ms = r.max_rtt_ms = s[0];
                double sum = 0;
                for (double v : s) {
                    sum += v;
                    if (v < r.min_rtt_ms) r.min_rtt_ms = v;
                    if (v > r.max_rtt_ms) r.max_rtt_ms = v;
                }
                r.avg_rtt_ms  = sum / r.samples;
                r.avg_one_way = r.avg_rtt_ms / 2.0;
            } else {
                no_samples = true;
            }
            cs->last_result = r;
            cb = cs->on_result;
            cb_any = on_any_latency_result;

            // ブラウザへ結果通知
            auto srv = server_ptr_;
            if (srv) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"latency_result\","
                    "\"avg_rtt\":%.1f,\"one_way\":%.1f,"
                    "\"min\":%.1f,\"max\":%.1f,\"samples\":%d}",
                    r.avg_rtt_ms, r.avg_one_way,
                    r.min_rtt_ms, r.max_rtt_ms, r.samples);
                for (auto& hdl : cs->conns) {
                    try { srv->send(hdl, std::string(buf),
                                       websocketpp::frame::opcode::text); }
                    catch (...) {}
                }
            }
        }
        if (no_samples) {
            blog(LOG_WARNING, "[obs-delay-stream] latency measure failed: sid=%s ch=%d (no pong)",
                 sid.c_str(), ch + 1);
        } else {
            blog(LOG_INFO, "[obs-delay-stream] latency measure done: sid=%s ch=%d samples=%d avg=%.1fms",
                 sid.c_str(), ch + 1, r.samples, r.avg_rtt_ms);
        }
        // コールバックはロック外で呼び出し
        if (cb) cb(sid, ch, r);
        if (cb_any) cb_any(sid, ch, r);
    }

    void broadcast_text(const std::string& sid, int ch, const std::string& msg) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto* cs = find_ch(sid, ch);
        auto srv = server_ptr_;
        if (!cs || !srv) return;
        for (auto& hdl : cs->conns) {
            try { srv->send(hdl, msg, websocketpp::frame::opcode::text); }
            catch (...) {}
        }
    }

    struct MeasureThread {
        std::thread th;
        std::atomic<bool> done{false};
    };

    std::shared_ptr<WsServer> server_ptr_;
    std::thread        thread_;
    mutable std::mutex mtx_;
    uint16_t           port_    = 0;
    std::atomic<bool>  running_{false};
    std::string        stream_id_;
    std::string        http_index_html_;
    std::string        http_root_dir_;
    std::atomic<int>   audio_codec_{0}; // 0: Opus, 1: PCM16
    std::atomic<bool>  opus_reset_pending_{false};
    std::atomic<int>   opus_bitrate_kbps_{96};
    std::vector<OpusEnc> opus_;

    // conn_handle → ConnInfo
    std::map<ConnHandle, ConnInfo, std::owner_less<ConnHandle>> conn_map_;
    // "stream_id/ch_0idx" → ChannelState
    std::map<std::string, ChannelState> ch_map_;
    std::array<std::string, MAX_CH> sub_memo_{};

    // stop() 時に退避される計測結果・適用遅延キャッシュ
    struct ChannelCache {
        LatencyResult last_result;
        double        last_applied_delay{-1.0};
    };
    std::map<std::string, ChannelCache> ch_cache_;

    // 計測スレッド管理
    std::mutex measure_threads_mtx_;
    std::vector<std::unique_ptr<MeasureThread>> measure_threads_;
};
