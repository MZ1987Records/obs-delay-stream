#pragma once

/**
 * 同期フローを管理する。
 *
 * WebSocket 計測: 接続中チャンネルを並列計測
 * RTSP E2E 計測: プローブ信号注入で終端到達遅延を計測
 *
 * マスターディレイは `max_latency + rtsp_e2e_latency` で算出する。
 */

#include "core/constants.hpp"
#include "network/rtsp-e2e-prober.hpp"     // RtspE2eProber, RtspE2eResult
#include "network/stream-router-types.hpp" // LatencyResult

#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>

// StreamRouter は .cpp 側で include するため前方宣言のみ。
namespace ods::network {
	class StreamRouter;
}

namespace ods::plugin {
	struct DelayStreamData;
}

namespace ods::sync {

	using ods::core::MAX_SUB_CH;
	using ods::core::DEFAULT_PING_COUNT;
	using ods::network::LatencyResult;
	using ods::network::RtspE2eProber;
	using ods::network::RtspE2eResult;

	/**
	 * 同期フローの状態。
	 */
	enum class FlowPhase {
		Idle,             ///< 待機中
		WsMeasuring,      ///< WebSocket 計測中: 全 CH 並列計測
		WsDone,           ///< WebSocket 計測完了（確認待ち）
		RtspE2eMeasuring, ///< RTSP E2E 計測中
		RtspE2eDone,      ///< RTSP E2E 計測完了（確認待ち）
		Complete,         ///< 全完了
	};

	/**
	 * チャンネル別計測結果サマリ。
	 */
	struct ChSummary {
		int    ch;                 ///< 0-indexed
		bool   connected;          ///< 接続中か
		bool   measured;           ///< 計測成功か
		double one_way_latency_ms; ///< 片道レイテンシ推定値
	};

	/**
	 * フロー全体の結果。
	 */
	struct FlowResult {
		std::array<ChSummary, MAX_SUB_CH> channels{}; ///< CH ごとの計測結果

		int completed_count  = 0; ///< 成功/失敗を問わず計測処理が完了した件数
		int ping_sent_count  = 0; ///< 送信済み ping 合計
		int ping_total_count = 0; ///< 予定 ping 総数

		double      rtsp_e2e_latency_ms     = 0.0;   ///< RTSP E2E 計測結果（中央値, ms）
		double      rtsp_e2e_min_latency_ms = 0.0;   ///< RTSP E2E 計測結果（最小値, ms）
		double      rtsp_e2e_max_latency_ms = 0.0;   ///< RTSP E2E 計測結果（最大値, ms）
		bool        rtsp_e2e_valid          = false; ///< RTSP E2E 計測結果が有効か
		std::string rtsp_e2e_error;                  ///< RTSP E2E 計測失敗時の理由
		int         rtsp_e2e_completed_sets = 0;     ///< RTSP E2E 計測の完了セット数
		int         rtsp_e2e_total_sets     = 0;     ///< RTSP E2E 計測の総セット数

		int connected_count() const {
			int count = 0;
			for (const auto &ch : channels) {
				if (ch.connected) ++count;
			}
			return count;
		}

		int measured_count() const {
			int count = 0;
			for (const auto &ch : channels) {
				if (ch.measured) ++count;
			}
			return count;
		}

		double max_latency_raw_ms() const {
			double max_ow = 0.0;
			for (const auto &ch : channels) {
				if (ch.measured && ch.one_way_latency_ms > max_ow) {
					max_ow = ch.one_way_latency_ms;
				}
			}
			return max_ow;
		}

		int max_latency_ms() const {
			return static_cast<int>(std::lround(max_latency_raw_ms()));
		}

		/// チャンネル i の計測レイテンシを整数 ms で返す（未計測は 0）。
		int ch_measured_ms(int ch_index) const {
			if (ch_index < 0 || ch_index >= MAX_SUB_CH) return 0;
			const auto &ch = channels[ch_index];
			if (!ch.measured) return 0;
			return static_cast<int>(std::lround(ch.one_way_latency_ms));
		}

		/// RTSP E2E 計測レイテンシを整数 ms で返す（無効時は 0）。
		int rtsp_e2e_ms() const {
			if (!rtsp_e2e_valid) return 0;
			return static_cast<int>(std::lround(rtsp_e2e_latency_ms));
		}
	};

	/**
	 * 同期フロー実行クラス。
	 */
	class SyncFlow {
	public:

		std::function<void()>                      on_update;            ///< GUI 再描画要求コールバック
		std::function<void()>                      on_progress;          ///< 計測進捗の軽量通知（再構築不要）
		std::function<void(int ch, LatencyResult)> on_ch_measured;       ///< 各 CH 計測完了通知
		std::function<void(int rtsp_e2e_ms)>       on_rtsp_e2e_measured; ///< RTSP E2E 計測結果通知
		std::function<void(const FlowResult &)>    on_ws_measured;       ///< WebSocket 計測完了時の全 CH 計測結果通知

		/// 初期状態へリセットして構築する。
		SyncFlow();

		/// WebSocket 計測で対象にする CH 数を設定する。
		void set_active_channels(int n);
		/// 各 CH 計測時の ping 送信回数を設定する。
		void set_ping_count(int n);

		/// 現在フェーズを取得する。
		FlowPhase phase() const;
		/// 最新の結果スナップショットを取得する。
		FlowResult result() const;
		/// 測定処理が進行中かを返す。
		bool busy() const;

		/// WebSocket 計測（各 CH レイテンシ計測）を開始する。
		/// @param skip_measured_ms  非 null の場合、要素 >= 0 の CH はスキップし結果をその値で埋める。
		bool start_ws_measurement(ods::network::StreamRouter &router, const std::string &stream_id, const int *skip_measured_ms = nullptr);
		/// RTSP E2E 計測を開始する。
		bool start_rtsp_e2e_measurement(const std::string            &rtsp_url,
										const std::string            &ffmpeg_path,
										ods::plugin::DelayStreamData &audio_data);
		/// RTSP E2E 計測結果を適用して完了状態へ進める。
		bool apply_rtsp_e2e_result();
		/// フロー全体の状態を初期化する。
		void reset();

	private:

		mutable std::mutex mtx_;                             ///< phase_/result_ などの排他制御
		FlowPhase          phase_      = FlowPhase::Idle;    ///< 現在フェーズ
		int                active_ch_  = MAX_SUB_CH;         ///< 計測対象 CH 数
		int                ping_count_ = DEFAULT_PING_COUNT; ///< CH ごとの ping 回数
		std::atomic<int>   pending_count_{0};                ///< 未完了 CH 計測件数
		std::atomic<int>   ping_sent_count_{0};              ///< 実送信 ping 件数
		FlowResult         result_;                          ///< フロー結果本体
		RtspE2eProber      prober_e2e_;                      ///< RTSP E2E 計測器

		/// 各 CH 計測完了時の集計処理。
		void on_ch_result(int i, LatencyResult r);
	};

} // namespace ods::sync
