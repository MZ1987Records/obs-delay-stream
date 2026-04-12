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

	/// websocketpp のサーバー型
	using WsServer = websocketpp::server<websocketpp::config::asio>;

	/// クライアント接続ハンドル型
	using ConnectionHandle = websocketpp::connection_hdl;

	/// RTT 計測に使う単調時計
	using Clock = std::chrono::steady_clock;

	/// 時間差をミリ秒(double)として扱う型
	using DurationMs = std::chrono::duration<double, std::milli>;

	/// 接続数変化通知コールバック（sid, ch, count）
	using ConnChangeCallback = std::function<void(const std::string &, int, size_t)>;

	/// owner_less 比較による接続ハンドル集合
	using ConnectionSet = std::set<ConnectionHandle, std::owner_less<ConnectionHandle>>;

	inline constexpr uint32_t MAGIC_AUDI = 0x41554449u; // "AUDI"
	inline constexpr uint32_t MAGIC_OPUS = 0x4F505553u; // "OPUS"

	/**
	 * RTT 計測結果の集計値。
	 */
	struct LatencyResult {
		bool        valid          = false; ///< 結果が有効か
		double      avg_rtt_ms     = 0.0;   ///< 平均 RTT (ms)
		double      avg_latency_ms = 0.0;   ///< 推定片道レイテンシ (ms)
		double      min_rtt_ms     = 0.0;   ///< 最小 RTT (ms)
		double      max_rtt_ms     = 0.0;   ///< 最大 RTT (ms)
		int         samples        = 0;     ///< 受信できたサンプル数（外れ値除外前）
		int         used_samples   = 0;     ///< 統計計算に使ったサンプル数
		const char *method         = "";    ///< "median" | "trimmed" | "iqr"
	};

	/// チャンネル別レイテンシ結果通知コールバック（sid, ch, result）
	using LatencyCallback = std::function<void(const std::string &, int, LatencyResult)>;

	/**
	 * WebSocket 接続ごとの識別情報。
	 */
	struct ConnInfo {
		std::string stream_id;         ///< 接続が属する配信 ID
		int         ch        = -1;    ///< 接続先チャンネル (0-indexed)
		bool        force_pcm = false; ///< この接続のみ PCM 強制配信するか
	};

	/// ハンドルをキーとする接続情報マップ
	using ConnectionMap = std::map<ConnectionHandle, ConnInfo, std::owner_less<ConnectionHandle>>;

	/**
	 * 音声配信のエンコード設定。
	 */
	struct AudioConfig {
		int  codec                   = 0;    ///< 0: Opus, 1: PCM
		int  opus_bitrate_kbps       = 96;   ///< Opus ビットレート (kbps)
		int  opus_target_sample_rate = 0;    ///< 0 は入力サンプルレートを使用
		int  quantization_bits       = 8;    ///< PCM 量子化ビット数
		bool mono                    = true; ///< true でモノラル化
		int  pcm_downsample_ratio    = 4;    ///< 1: そのまま, 2: 1/2, 4: 1/4
		int  playback_buffer_ms      = 0;    ///< クライアント再生バッファ目標 (ms)
	};

	/**
	 * 配信ID + チャンネル単位の状態。
	 */
	struct ChannelState {
		ConnectionSet conns; ///< 接続中クライアント

		std::map<int, Clock::time_point> ping_times;       ///< 送信済み ping の時刻（seq -> time）
		std::vector<double>              rtt_samples;      ///< 収集中 RTT サンプル
		std::atomic<bool>                measuring{false}; ///< RTT 計測中フラグ
		LatencyResult                    last_result;      ///< 直近計測結果

		double      last_applied_delay{-1.0}; ///< 最後に通知したディレイ値
		std::string last_applied_reason;      ///< ディレイ通知の理由

		LatencyCallback on_result; ///< チャンネル別結果通知
	};

} // namespace ods::network
