#pragma once

#include "model/delay-state.hpp"
#include "plugin/plugin-settings.hpp"

#include <obs-module.h>
#include <climits>
#include <string>
#include <vector>

namespace ods::viewmodel {

	using ods::model::DelaySnapshot;
	using ods::model::DelayState;

	/**
	 * ディレイタブ (tab 5) の表示に必要なデータを事前計算した読み取り専用スナップショット。
	 *
	 * get_properties() の冒頭で build() し、UI 構築関数に const 参照で渡す。
	 * 生の DelayStreamData や obs_data_t への直接アクセスを排除し、
	 * 「何を表示するか」をこの構造体だけで決定できるようにする。
	 */
	struct DelayViewModel {
		/// チャンネルごとの表示用データ。
		struct ChDisplay {
			std::string name;        ///< メモ名（空文字列可）
			float       measured_ms; ///< WS 配信遅延（片道 ms）。未計測は -1.0f
			int         offset_ms;   ///< 想定環境遅延（チャンネル別補正オフセット）
			int         total_ms;    ///< 自動調整ディレイ（バッファ適用値）
			bool        provisional; ///< 仮値（他チャンネルの最小計測値）を使用中か
		};

		DelaySnapshot          snapshot;               ///< 全チャンネルディレイ計算結果
		std::vector<ChDisplay> channels;               ///< チャンネルごとの表示データ
		int                    selected_ch;            ///< テーブルで選択中のチャンネル
		int                    rtsp_e2e_ms        = 0; ///< R: OBS 配信レイテンシ (ms)
		int                    avatar_latency_ms  = 0; ///< A: アバターレイテンシ (ms)
		int                    playback_buffer_ms = 0; ///< B: 再生バッファ (ms)

		/// DelayState と obs_data から表示用 ViewModel を構築する。
		static DelayViewModel build(const DelayState &delay, obs_data_t *settings) {
			DelayViewModel vm;
			vm.snapshot = delay.calc_all_delays();

			const int sub_count = vm.snapshot.active_count;

			vm.rtsp_e2e_ms        = delay.measured_rtsp_e2e_ms;
			vm.avatar_latency_ms  = delay.avatar_latency_ms;
			vm.playback_buffer_ms = delay.playback_buffer_ms;

			vm.selected_ch = 0;
			if (settings) {
				vm.selected_ch = static_cast<int>(obs_data_get_int(settings, "delay_table_selected_ch"));
				if (vm.selected_ch < 0 || vm.selected_ch >= sub_count)
					vm.selected_ch = 0;
			}

			// 仮値の基準: 計測済みチャンネルの最小 WS 遅延
			int min_measured = INT_MAX;
			for (int i = 0; i < sub_count; ++i) {
				if (delay.channels[i].ws_measured && delay.channels[i].measured_ms < min_measured)
					min_measured = delay.channels[i].measured_ms;
			}

			vm.channels.resize(static_cast<size_t>(sub_count));
			for (int i = 0; i < sub_count; ++i) {
				const auto &sch = vm.snapshot.channels[i];
				auto       &ch  = vm.channels[i];

				const auto  memo_key = ods::plugin::make_sub_memo_key(i);
				const char *memo     = settings ? obs_data_get_string(settings, memo_key.data()) : "";
				ch.name              = (memo && *memo) ? memo : "";
				ch.provisional       = sch.provisional;
				if (sch.has_measurement) {
					ch.measured_ms = static_cast<float>(delay.channels[i].measured_ms);
					ch.total_ms    = sch.total_ms;
				} else if (sch.provisional) {
					ch.measured_ms = static_cast<float>(min_measured);
					ch.total_ms    = sch.total_ms;
				} else {
					ch.measured_ms = -1.0f;
					ch.total_ms    = 0;
				}
				ch.offset_ms = delay.channels[i].offset_ms;
			}

			return vm;
		}
	};

} // namespace ods::viewmodel
