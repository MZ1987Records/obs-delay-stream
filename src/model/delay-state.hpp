#pragma once

#include "core/constants.hpp"

#include <array>

namespace ods::model {

	using ods::core::MAX_SUB_CH;

	/**
	 * 全チャンネルの遅延計算結果スナップショット。
	 *
	 * UI 表示と DelayBuffer 適用の両方で使い、計算ロジックの重複を排除する。
	 */
	struct DelaySnapshot {
		struct ChDelay {
			int  raw_ms          = 0;     ///< R - A - C[i] - B + offset[i]（負値許容）
			int  total_ms        = 0;     ///< raw_ms + neg_max（バッファ適用値）
			bool has_measurement = false; ///< WS 計測済みか
			bool warn            = false; ///< floor 補正の原因チャンネルか
		};
		std::array<ChDelay, MAX_SUB_CH> channels{};
		int                             neg_max_ms      = 0; ///< 負値 raw の最大絶対値
		int                             master_delay_ms = 0; ///< OBS 出力への遅延 = neg_max
		int                             active_count    = 0; ///< 計算対象チャンネル数
	};

	/**
	 * 遅延計算に必要な入力値をまとめた状態構造体。
	 *
	 * DelayStreamData から遅延計算の関心事だけを分離し、
	 * calc_all_delays() で一元的にスナップショットを生成する。
	 */
	struct DelayState {
		/// チャンネルごとの遅延計算入力値。
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

		std::array<ChDelay, MAX_SUB_CH> channels{}; ///< チャンネルごとの遅延入力

		/// 1 チャンネルの補正前遅延値を計算する。
		static int calc_ch_raw_delay_ms(int rtsp_e2e_ms,
										int avatar_latency_ms,
										int ch_measured_ms,
										int playback_buffer_ms,
										int offset_ms) {
			return rtsp_e2e_ms - avatar_latency_ms - ch_measured_ms
				   - playback_buffer_ms + offset_ms;
		}

		/// 全チャンネルの遅延を一括計算してスナップショットを返す。
		DelaySnapshot calc_all_delays() const {
			DelaySnapshot snap;
			snap.active_count = sub_ch_count;

			const int R = measured_rtsp_e2e_ms;
			const int A = avatar_latency_ms;
			const int B = playback_buffer_ms;

			for (int i = 0; i < sub_ch_count; ++i) {
				auto &out           = snap.channels[i];
				out.has_measurement = channels[i].ws_measured;
				if (!out.has_measurement) {
					out.raw_ms = 0;
					continue;
				}
				out.raw_ms = calc_ch_raw_delay_ms(R, A, channels[i].measured_ms, B, channels[i].offset_ms);
				if (out.raw_ms < 0 && -out.raw_ms > snap.neg_max_ms) {
					snap.neg_max_ms = -out.raw_ms;
				}
			}

			for (int i = 0; i < sub_ch_count; ++i) {
				auto &out = snap.channels[i];
				if (out.has_measurement) {
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
