#pragma once

/**
 * RTSP ストリームを FFmpeg サブプロセスで受信し、
 * 注入プローブ信号の到達時刻から E2E 遅延を算出する。
 */

#include "audio/probe-signal.hpp"
#include "core/constants.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ods::network {

	/**
	 * RTSP E2E 計測結果。
	 */
	struct RtspE2eResult {
		bool        valid          = false; ///< 計測成功フラグ
		double      latency_ms     = 0.0;   ///< E2E 片道レイテンシ推定値（中央値, ms）
		double      min_latency_ms = 0.0;   ///< 試行結果の最小値 (ms)
		double      max_latency_ms = 0.0;   ///< 試行結果の最大値 (ms)
		std::string error_msg;              ///< 失敗理由
	};

	/**
	 * RTSP E2E 計測ワーカー。
	 */
	class RtspE2eProber {
	public:

		std::function<void()>              on_ready;    ///< FFmpeg が受信開始したら呼ばれる
		std::function<void(int, int)>      on_progress; ///< セット進捗通知（完了数, 総数）
		std::function<void(RtspE2eResult)> on_result;   ///< 検出完了または失敗時に呼ばれる

		RtspE2eProber();  ///< 既定状態で構築する
		~RtspE2eProber(); ///< 実行中の計測を停止して破棄する

		/// 自動配置先の ffmpeg.exe が存在する場合に絶対パスを返す。
		static bool get_auto_ffmpeg_path_if_exists(std::string &out);
		/// 自動配置先へ ffmpeg.exe を配置し、利用可能な絶対パスを返す。
		static bool ensure_auto_ffmpeg_path(std::string &out, std::string &err);

		/// RTSP E2E 計測を開始する。
		bool start(const std::string &rtsp_url, const std::string &ffmpeg_path_hint);
		/// プローブ注入完了時刻を通知する。
		void notify_impulse_sent(std::chrono::steady_clock::time_point t0);
		/// 実行中の計測をキャンセルする。
		void cancel();
		/// 計測ワーカーが実行中か返す。
		bool is_running() const;
		/// 直近の計測結果を返す。
		RtspE2eResult last_result() const;

	private:

		void worker_loop();
		RtspE2eResult run_single_probe(const std::string &ffmpeg_exe_path);

		std::string        rtsp_url_;                                                ///< 計測対象 RTSP URL
		std::string        ffmpeg_path_hint_;                                        ///< ffmpeg 実行パス指定（"auto" / "%PATH%" / 絶対パス）
		std::atomic<bool>  running_{false};                                          ///< 実行中フラグ
		int                measure_sets_ = ods::core::RTSP_E2E_MEASURE_SETS_DEFAULT; ///< 接続・計測・切断の反復回数
		std::thread        worker_;                                                  ///< 計測ワーカースレッド
		mutable std::mutex mtx_;                                                     ///< 共有結果の排他制御
		RtspE2eResult      last_result_;                                             ///< 直近計測結果

		std::atomic<bool>                     impulse_sent_{false}; ///< プローブ送信済みフラグ
		std::chrono::steady_clock::time_point t0_{};                ///< プローブ注入時刻
		std::mutex                            t0_mtx_;              ///< t0_ の排他制御

		ods::audio::ProbeSignal   probe_signal_; ///< チャープリファレンス
		ods::audio::ProbeDetector detector_;     ///< チャープ相関検出器
	};

} // namespace ods::network
