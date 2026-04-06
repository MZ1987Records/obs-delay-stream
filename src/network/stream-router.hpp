#pragma once

/*
 * stream-router.hpp
 *
 * StreamRouter: 単一ポート(WS_PORT)でパスルーティングを行う WebSocketサーバー
 *
 * 接続URL: ws://[IP]:[PORT]/[配信ID]/[ch番号(1-20)]
 *   例: ws://192.168.1.10:19000/myshow2024/1
 *
 * 配信IDが一致するchへのみ音声・pingを届ける。
 * 配信IDが異なる接続は別セッションとして完全に分離される。
 *
 * 【音声バイナリフォーマット】
 *   PCM  : [4B magic=0x41554449][4B SR][4B CH][4B frames][int16*frames*CH]
 *   OPUS : [4B magic=0x4F505553][4B SR][4B CH][4B frames][Opus packet bytes]
 *
 * 【制御メッセージ (テキストJSON)】
 *   OBS → Browser: {"type":"ping","seq":N,"t":T}
 *   Browser → OBS: {"type":"pong","seq":N}
 *   OBS → Browser: {"type":"latency_result","avg_rtt":X,"one_way":Y,"min":A,"max":B,"samples":N}
 *   OBS → Browser: {"type":"apply_delay","ms":X,"reason":"auto_measure|manual_adjust"}
 *   OBS → Browser: {"type":"session_info","stream_id":"xxx","ch":N,"memo":"..."}  ← 接続直後に送信
 *   OBS → Browser: {"type":"playback_buffer","ms":X}  ← 接続直後/設定変更時に送信
 *   OBS → Browser: {"type":"memo","ch":N,"memo":"..."}  ← メモ変更通知
 *   Browser → OBS: {"type":"audio_codec","mode":"pcm"}  ← Opus不可時のPCM要求
 *   Browser → OBS: {"type":"audio_codec","mode":"opus","bitrate_kbps":96,"sample_rate":48000}
 *
 * v2.0 changes:
 *   - send_audio() を非ブロッキング化 (ASIO::post 経由で送信)
 *   - .detach() を廃止、全計測スレッドを join() 管理
 *   - stop() の正しい停止順序
 */

#define ASIO_STANDALONE
#define _WEBSOCKETPP_CPP11_STL_

// Prevent SIMDe from redefining standard integer types
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

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/constants.hpp"
#include "network/opus-encoder.hpp"
#include "network/stream-router-types.hpp"
#include "network/stream-router-utils.hpp"

// ============================================================
// StreamRouter
//   全chを1ポートで管理。配信ID + ch番号でルーティング。
// ============================================================
class StreamRouter {
	public:
	StreamRouter() = default;
	~StreamRouter() { stop(); }

	// 接続数の変化通知 (sid, ch_0idx, count)
	std::function<void(const std::string &, int, size_t)> on_conn_change;
	// 計測結果通知（sid, ch_0idx, result）
	std::function<void(const std::string &, int, LatencyResult)> on_any_latency_result;
	// ping送信通知（sid, ch_0idx, seq）
	std::function<void(const std::string &, int, int)> on_any_ping_sent;

	// ----- 起動 / 停止 -----
	bool start(uint16_t port);
	void stop();

	// ----- 配信ID設定 -----
	void        set_stream_id(const std::string &id);
	std::string stream_id() const;

	void set_active_channels(int n);
	int  active_channels() const;

	void set_sub_memo(int ch, const std::string &memo);

	// ----- チャンネル識別コード -----
	void        set_sub_code(int ch, const std::string &code);
	std::string sub_code(int ch) const;
	// 識別コードからチャンネル(0-indexed)を解決。見つからなければ -1。
	int resolve_code(const std::string &code) const;

	// ----- 音声送信 (ch: 0-indexed) -----
	// 音声スレッドから呼ばれる。非ブロッキング: ASIO スレッドに委譲。
	void send_audio(int ch, const float *data, size_t frames, uint32_t sample_rate, uint32_t channels);

	// ----- RTT計測 (ch: 0-indexed) -----
	bool          start_measurement(int ch, int num_pings = DEFAULT_PING_COUNT, int interval_ms = 150, int start_delay_ms = 0);
	bool          is_measuring(int ch) const;
	LatencyResult last_result(int ch) const;

	// 計測完了コールバック設定 (ch: 0-indexed)
	void set_on_latency_result(int                                                          ch,
							   std::function<void(const std::string &, int, LatencyResult)> cb);

	void set_http_index_html(std::string html);

	// 全コールバックをクリア（破棄前に呼ぶ）
	void clear_callbacks();

	// 遅延反映通知
	void notify_apply_delay(int ch, double ms, const char *reason = "auto_measure");

	// 接続数取得 (ch: 0-indexed)
	size_t client_count(int ch) const;

	// 受信URLを生成
	std::string make_url(const std::string &host, int ch_1indexed) const;

	uint16_t port() const { return port_; }
	bool     is_running() const { return running_; }

	// ----- 音声コーデック設定 -----
	void set_audio_config(const AudioConfig &cfg);
	void set_http_root_dir(std::string dir);

	private:
	using OpusEnc         = websocket_server_detail::OpusEnc;
	using PathParseResult = websocket_server_detail::PathParseResult;

	static std::string     make_key(const std::string &sid, int ch);
	static std::string     sanitize_id(const std::string &raw);
	static std::string     json_escape(const std::string &s);
	static std::string     url_decode(const std::string &s);
	static PathParseResult parse_path_code(const std::string &path,
										   std::string       &stream_id,
										   std::string       &code);
	static bool            is_safe_rel_path(const std::string &rel);
	static std::string     join_path(const std::string &base, const std::string &rel);
	static bool            read_file_to_string(const std::string &path, std::string &out);
	static const char     *guess_content_type(const std::string &path);
	static bool            is_valid_opus_sample_rate(int sample_rate);
	static bool            is_valid_pcm_downsample_ratio(int r);

	// 三角形フィルタ [0.25, 0.5, 0.25] による 1/2 間引きを段階適用
	// factor=2: 1 段、factor=4: 2 段カスケード（等価 7 タップ FIR）
	static void        downsample_pcm(const float *in, size_t in_frames, uint32_t channels, int factor, std::vector<float> &out, size_t &out_frames);
	static float       quantize_sample(float v, int bits);
	static std::string format_latency_result_json(const LatencyResult &r);

	ChannelState       *find_ch(const std::string &sid, int ch);
	const ChannelState *find_ch(const std::string &sid, int ch) const;
	void                join_all_measure_threads();
	void                cleanup_done_measure_threads();

	void preprocess_audio(const float *in_data, size_t frames, uint32_t in_channels, const float *&out_data, uint32_t &out_channels, std::vector<float> &work);
	bool ensure_opus_encoder(int ch, uint32_t sample_rate, uint32_t channels);
	void reset_opus_state();
	bool encode_opus_packets(int ch, const float *data, size_t frames, uint32_t sample_rate, uint32_t channels, std::vector<std::shared_ptr<std::string>> &out);

	void on_open(ConnHandle h);
	void on_close(ConnHandle h);
	void on_message(ConnHandle h, WsServer::message_ptr msg);
	void on_http(ConnHandle h);

	void measure_loop(const std::string &sid, int ch, int num_pings, int interval_ms, int start_delay_ms);
	void finalize_result(const std::string &sid, int ch);
	void broadcast_text(const std::string &sid, int ch, const std::string &msg);

	struct MeasureThread {
		std::thread       th;
		std::atomic<bool> done{false};
	};

	struct ChannelCache {
		LatencyResult last_result;
		double        last_applied_delay{-1.0};
		std::string   last_applied_reason;
	};

	std::shared_ptr<WsServer> server_ptr_;
	std::thread               thread_;
	mutable std::mutex        mtx_;
	uint16_t                  port_ = 0;
	std::atomic<bool>         running_{false};
	std::string               stream_id_;
	std::string               http_index_html_;
	std::string               http_root_dir_;
	std::atomic<int>          audio_codec_{0}; // 0: Opus, 1: PCM
	std::atomic<bool>         opus_reset_pending_{false};
	std::atomic<bool>         opus_flush_pending_{false};
	std::atomic<int>          opus_bitrate_kbps_{96};
	std::atomic<int>          opus_target_sample_rate_{0}; // 0: source sample rate
	std::atomic<int>          audio_quantization_bits_{8};
	std::atomic<bool>         audio_mono_{true};
	std::atomic<int>          pcm_downsample_ratio_{4}; // 1: そのまま, 2: 1/2, 4: 1/4
	std::atomic<int>          playback_buffer_ms_{PLAYBACK_BUFFER_DEFAULT_MS};
	std::atomic<uint64_t>     pb_debounce_seq_{0};
	std::atomic<bool>         pb_debounce_running_{false};
	std::vector<OpusEnc>      opus_;
	int                       active_ch_max_ = MAX_SUB_CH;

	// conn_handle → ConnInfo
	std::map<ConnHandle, ConnInfo, std::owner_less<ConnHandle>> conn_map_;
	// "stream_id/ch_0idx" → ChannelState
	std::map<std::string, ChannelState> ch_map_;
	std::array<std::string, MAX_SUB_CH> sub_memo_{};
	std::array<std::string, MAX_SUB_CH> sub_code_{};

	// stop() 時に退避される計測結果・適用遅延キャッシュ
	std::map<std::string, ChannelCache> ch_cache_;

	// 計測スレッド管理
	std::mutex                                  measure_threads_mtx_;
	std::vector<std::unique_ptr<MeasureThread>> measure_threads_;
};
