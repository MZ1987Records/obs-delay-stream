#pragma once

/**
 * 同期フローを管理する。
 *
 * WebSocket 計測: 接続中チャンネルを並列計測
 * RTMP 計測: RTMP レイテンシ計測
 *
 * マスター遅延は `max_latency + rtmp_latency` で算出する。
 */

#include "core/constants.hpp"
#include "network/rtmp-prober.hpp"         // RtmpProber, RtmpProbeResult
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

namespace ods::sync {

	using ods::core::MAX_SUB_CH;
	using ods::core::DEFAULT_PING_COUNT;
	using ods::network::LatencyResult;
	using ods::network::RtmpProber;
	using ods::network::RtmpProbeResult;

	/**
	 * 同期フローの状態。
	 */
	enum class FlowPhase {
		Idle,          ///< 待機中
		WsMeasuring,   ///< WebSocket 計測中: 全 CH 並列計測
		WsDone,        ///< WebSocket 計測完了（確認待ち）
		RtmpMeasuring, ///< RTMP 計測中
		RtmpDone,      ///< RTMP 計測完了（確認待ち）
		Complete,      ///< 全完了
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

		double      rtmp_latency_ms = 0.0;   ///< RTMP 計測で得た平均レイテンシ
		bool        rtmp_valid      = false; ///< RTMP 計測結果が有効か
		std::string rtmp_error;              ///< RTMP 計測失敗時の理由

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

		int proposed_sub_delay_ms(int ch_index) const {
			if (ch_index < 0 || ch_index >= MAX_SUB_CH) return 0;
			const auto &ch = channels[ch_index];
			if (!ch.measured) return 0;
			double proposed = max_latency_raw_ms() - ch.one_way_latency_ms;
			if (proposed < 0.0) proposed = 0.0;
			return static_cast<int>(std::lround(proposed));
		}

		int proposed_master_delay_ms() const {
			double proposed = max_latency_raw_ms() + rtmp_latency_ms;
			if (proposed < 0.0) proposed = 0.0;
			return static_cast<int>(std::lround(proposed));
		}
	};

	/**
	 * 同期フロー実行クラス。
	 */
	class SyncFlow {
	public:

		std::function<void()>                      on_update;                ///< GUI 再描画要求コールバック
		std::function<void()>                      on_progress;              ///< ping 送信ごとの軽量進捗通知（再構築不要）
		std::function<void(int ch, LatencyResult)> on_ch_measured;           ///< 各 CH 計測完了通知
		std::function<void(int master_ms)>         on_apply_master;          ///< マスター遅延書き込み要求
		std::function<void(const FlowResult &)>    on_apply_sub_base_delays; ///< WebSocket 計測完了時の全 CH 遅延書き込み要求

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
		bool start_ws_measurement(ods::network::StreamRouter &router, const std::string &stream_id);
		/// WebSocket 計測で失敗した CH のみ再計測する。
		bool retry_failed_channels(ods::network::StreamRouter &router);
		/// RTMP 計測を開始する。
		bool start_rtmp_measurement(const std::string &rtmp_url);
		/// RTMP 計測結果を適用して完了状態へ進める。
		bool apply_rtmp_result();
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
		RtmpProber         prober_;                          ///< RTMP レイテンシ計測器

		/// 各 CH 計測完了時の集計処理。
		void on_ch_result(int i, LatencyResult r);
	};

} // namespace ods::sync
