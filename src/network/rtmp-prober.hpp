#pragma once

/**
 * RTMP サーバーへの計測専用 TCP 接続を張り、
 * ハンドシェイク (C0+C1 -> S0+S1+S2) の RTT から
 * ネットワーク片道レイテンシを推定する。
 *
 * 実際の配信接続には干渉しない。
 */

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ods::network {

	/**
	 * RTMP プローブ結果。
	 */
	struct RtmpProbeResult {
		bool        valid          = false; ///< 結果が有効か
		double      avg_rtt_ms     = 0.0;   ///< 平均 RTT (ms)
		double      avg_latency_ms = 0.0;   ///< 推定片道レイテンシ = RTT / 2
		double      min_rtt_ms     = 0.0;   ///< 最小 RTT (ms)
		double      max_rtt_ms     = 0.0;   ///< 最大 RTT (ms)
		double      jitter_ms      = 0.0;   ///< RTT 標準偏差
		int         samples        = 0;     ///< 成功サンプル数
		int         failed         = 0;     ///< 失敗した試行数
		std::string error_msg;              ///< 失敗時の理由
	};

	/**
	 * RTMP RTT プローブ実行クラス。
	 */
	class RtmpProber {
	public:

		std::function<void(RtmpProbeResult)> on_result; ///< 計測完了コールバック。ワーカースレッドから呼ばれる

		/// 既定状態で構築する。
		RtmpProber();
		/// 実行中の計測を停止して破棄する。
		~RtmpProber();

		/// RTMP 計測を開始する。
		/// @param rtmp_url "rtmp://example.com/live/key" または "example.com:1935"
		bool start(const std::string &rtmp_url,
				   int                num_probes  = 10,
				   int                interval_ms = 300);
		/// 実行中の計測をキャンセルする。
		void cancel();

		/// 計測ワーカーが動作中かを返す。
		bool is_running() const;
		/// 直近の計測結果を返す。
		RtmpProbeResult last_result() const;
		/// 現在の接続先ホスト名を返す。
		std::string host() const;
		/// 現在の接続先ポートを返す。
		int port() const;

	private:

		std::string        host_;               ///< 計測先ホスト
		int                port_        = 1935; ///< 計測先ポート
		int                num_probes_  = 10;   ///< 試行回数
		int                interval_ms_ = 300;  ///< 試行間隔 (ms)
		std::atomic<bool>  running_{false};     ///< 実行中フラグ
		std::thread        worker_;             ///< 計測ワーカースレッド
		mutable std::mutex mtx_;                ///< 共有結果の排他制御
		RtmpProbeResult    last_result_;        ///< 直近計測結果

		/// RTMP URL/ホスト指定を host+port へ分解する。
		static bool parse_url(const std::string &url, std::string &host, int &port);
		/// 1 回分の RTT 計測を実行する。
		double probe_once();
		/// 複数回計測ループを実行して結果を集約する。
		void probe_loop();
	};

} // namespace ods::network
