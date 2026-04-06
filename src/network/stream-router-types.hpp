#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

namespace ods::network {

using WsServer   = websocketpp::server<websocketpp::config::asio>;
using ConnHandle = websocketpp::connection_hdl;
using Clock      = std::chrono::steady_clock;
using Ms         = std::chrono::duration<double, std::milli>;

inline constexpr uint32_t MAGIC_AUDI = 0x41554449u; // "AUDI"
inline constexpr uint32_t MAGIC_OPUS = 0x4F505553u; // "OPUS"

struct LatencyResult {
	bool        valid          = false;
	double      avg_rtt_ms     = 0.0;
	double      avg_latency_ms = 0.0;
	double      min_rtt_ms     = 0.0;
	double      max_rtt_ms     = 0.0;
	int         samples        = 0;  // 受信できたサンプル数（外れ値除外前）
	int         used_samples   = 0;  // 統計計算に使ったサンプル数
	const char *method         = ""; // "median" | "trimmed" | "iqr"
};

struct ConnInfo {
	std::string stream_id;
	int         ch        = -1;
	bool        force_pcm = false;
};

struct AudioConfig {
	// 0: Opus, 1: PCM
	int audio_codec_mode  = 0;
	int opus_bitrate_kbps = 96;
	// 0 は入力サンプルレートを使用
	int  opus_target_sample_rate = 0;
	int  quantization_bits       = 8;
	bool mono                    = true;
	// 1: そのまま, 2: 1/2, 4: 1/4
	int pcm_downsample_ratio = 4;
	int playback_buffer_ms   = 0;
};

struct ChannelState {
	std::set<ConnHandle, std::owner_less<ConnHandle>> conns;

	std::map<int, Clock::time_point> ping_times;
	std::vector<double>              rtt_samples;
	std::atomic<bool>                measuring{false};
	LatencyResult                    last_result;

	double      last_applied_delay{-1.0};
	std::string last_applied_reason;

	std::function<void(const std::string &stream_id, int ch, LatencyResult)> on_result;
};

} // namespace ods::network
