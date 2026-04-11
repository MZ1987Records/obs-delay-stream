#pragma once

#include "model/delay-state.hpp"
#include "plugin/plugin-settings.hpp"

#include <obs-module.h>
#include <string>
#include <vector>

namespace ods::viewmodel {

	using ods::model::DelaySnapshot;
	using ods::model::DelayState;

	/**
	 * 遅延タブ (tab 5) の表示に必要なデータを事前計算した読み取り専用スナップショット。
	 *
	 * get_properties() の冒頭で build() し、UI 構築関数に const 参照で渡す。
	 * 生の DelayStreamData や obs_data_t への直接アクセスを排除し、
	 * 「何を表示するか」をこの構造体だけで決定できるようにする。
	 */
	struct DelayViewModel {
		/// チャンネルごとの表示用データ。
		struct ChDisplay {
			std::string name;         ///< メモ名（空文字列可）
			float       measured_ms;  ///< 計測値（片道 ms）。未計測は -1.0f
			int         offset_ms;    ///< チャンネル別補正オフセット
			int         raw_delay_ms; ///< R - A - C[i] - B + offset[i]
			int         neg_max_ms;   ///< 負値フロア補正量（全チャンネル共通）
			int         total_ms;     ///< raw_delay_ms + neg_max_ms
			bool        warn;         ///< floor 補正の原因チャンネルか
		};

		DelaySnapshot          snapshot;    ///< 全チャンネル遅延計算結果
		std::vector<ChDisplay> channels;    ///< チャンネルごとの表示データ
		int                    selected_ch; ///< テーブルで選択中のチャンネル

		/// DelayState と obs_data から表示用 ViewModel を構築する。
		static DelayViewModel build(const DelayState &delay, obs_data_t *settings) {
			DelayViewModel vm;
			vm.snapshot = delay.calc_all_delays();

			const int sub_count = vm.snapshot.active_count;

			vm.selected_ch = 0;
			if (settings) {
				vm.selected_ch = static_cast<int>(obs_data_get_int(settings, "delay_table_selected_ch"));
				if (vm.selected_ch < 0 || vm.selected_ch >= sub_count)
					vm.selected_ch = 0;
			}

			vm.channels.resize(static_cast<size_t>(sub_count));
			for (int i = 0; i < sub_count; ++i) {
				const auto &sch = vm.snapshot.channels[i];
				auto       &ch  = vm.channels[i];

				const auto  memo_key = ods::plugin::make_sub_memo_key(i);
				const char *memo     = settings ? obs_data_get_string(settings, memo_key.data()) : "";
				ch.name              = (memo && *memo) ? memo : "";
				ch.measured_ms       = sch.has_measurement ? static_cast<float>(delay.channels[i].measured_ms) : -1.0f;
				ch.offset_ms         = delay.channels[i].offset_ms;
				ch.raw_delay_ms      = sch.has_measurement ? sch.raw_ms : 0;
				ch.neg_max_ms        = vm.snapshot.neg_max_ms;
				ch.total_ms          = sch.has_measurement ? sch.total_ms : 0;
				ch.warn              = sch.warn;
			}

			return vm;
		}
	};

} // namespace ods::viewmodel
