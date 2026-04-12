#pragma once

#include "core/constants.hpp"

#include <array>
#include <climits>

namespace ods::model {

	using ods::core::MAX_SUB_CH;

	/**
	 * 全チャンネルのディレイ計算結果スナップショット。
	 *
	 * UI 表示と DelayBuffer 適用の両方で使い、計算ロジックの重複を排除する。
	 */
	struct DelaySnapshot {
		struct ChDelay {
			int  raw_ms          = 0;     ///< R - A - C[i] - B - offset[i]（負値許容）
			int  total_ms        = 0;     ///< raw_ms + neg_max（バッファ適用値）
			bool has_measurement = false; ///< WS 計測済みか
			bool provisional     = false; ///< 仮値（他チャンネルの最小計測値）を使用中か
			bool warn            = false; ///< floor 補正の原因チャンネルか
		};
		std::array<ChDelay, MAX_SUB_CH> channels{};
		int                             neg_max_ms      = 0; ///< 負値 raw の最大絶対値
		int                             master_delay_ms = 0; ///< OBS 出力へのディレイ = neg_max
		int                             active_count    = 0; ///< 計算対象チャンネル数
	};

	/**
	 * ディレイ計算に必要な入力値をまとめた状態構造体。
	 *
	 * DelayStreamData からディレイ計算の関心事だけを分離し、
	 * calc_all_delays() で一元的にスナップショットを生成する。
	 */
	struct DelayState {
		/// チャンネルごとのディレイ計算入力値。
		struct ChDelay {
			int  measured_ms = 0;     ///< WS 計測結果の片道レイテンシ (ms)
			bool ws_measured = false; ///< WS 計測済みフラグ
			int  offset_ms   = 0;     ///< 手動補正オフセット (ms)
		};

		int  measured_rtsp_e2e_ms = 0;     ///< RTSP E2E 計測結果 (ms, OBS 設定に永続保存)
		bool rtsp_e2e_measured    = false; ///< RTSP E2E 計測済みフラグ
		int  avatar_latency_ms    = 0;     ///< アバター同期レイテンシ (ms, 0-5000)
		int  playback_buffer_ms   = 0;     ///< 再生バッファ量 (ms)
		int  sub_ch_count         = 1;     ///< アクティブなサブチャンネル数

		std::array<ChDelay, MAX_SUB_CH> channels{}; ///< チャンネルごとのディレイ計算入力

		/// 1 チャンネルの補正前ディレイ値を計算する。
		static int calc_ch_raw_delay_ms(int rtsp_e2e_ms,
										int avatar_latency_ms,
										int ch_measured_ms,
										int playback_buffer_ms,
										int offset_ms) {
			return rtsp_e2e_ms - avatar_latency_ms - ch_measured_ms - playback_buffer_ms - offset_ms;
		}

		/// 全チャンネルのディレイを一括計算してスナップショットを返す。
		DelaySnapshot calc_all_delays() const {
			DelaySnapshot snap;
			snap.active_count = sub_ch_count;

			const int R = measured_rtsp_e2e_ms;
			const int A = avatar_latency_ms;
			const int B = playback_buffer_ms;

			// 計測済みチャンネルの最小 WS 遅延を求める（仮値の基準）
			int min_measured = INT_MAX;
			for (int i = 0; i < sub_ch_count; ++i) {
				if (channels[i].ws_measured && channels[i].measured_ms < min_measured)
					min_measured = channels[i].measured_ms;
			}
			const bool has_any = (min_measured != INT_MAX);

			for (int i = 0; i < sub_ch_count; ++i) {
				auto &out           = snap.channels[i];
				out.has_measurement = channels[i].ws_measured;
				if (!out.has_measurement) {
					if (!has_any) {
						out.raw_ms = 0;
						continue;
					}
					// 仮値として最小計測値を使用
					out.provisional = true;
					out.raw_ms      = calc_ch_raw_delay_ms(R, A, min_measured, B, channels[i].offset_ms);
				} else {
					out.raw_ms = calc_ch_raw_delay_ms(R, A, channels[i].measured_ms, B, channels[i].offset_ms);
				}
				if (out.raw_ms < 0 && -out.raw_ms > snap.neg_max_ms)
					snap.neg_max_ms = -out.raw_ms;
			}

			for (int i = 0; i < sub_ch_count; ++i) {
				auto &out = snap.channels[i];
				if (out.has_measurement || out.provisional) {
					int val      = out.raw_ms + snap.neg_max_ms;
					out.total_ms = (val > 0) ? val : 0;
					out.warn     = (snap.neg_max_ms > 0 && out.raw_ms == -snap.neg_max_ms);
				}
			}

			snap.master_delay_ms = snap.neg_max_ms;
			return snap;
		}
	};

} // namespace ods::model
